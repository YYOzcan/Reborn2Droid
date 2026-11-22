/* Shared definitions for recompiled ARM code (libunity / libmono) and the runtime.
 *
 * Guest memory is one 4 GB host reservation starting at g_mem: guest address a lives at g_mem + a.
 * Every recompiled translation unit defines BASE (load address of its library) before including this.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPU {
    uint32_t r[16];
    uint32_t n, z, cf, v;            /* APSR flags, 0/1 */
    uint32_t fpscr;
    uint32_t _pad;
    union {                          /* VFP/NEON: s0-s31 alias d0-d15; d16-d31; q0-q15 alias d pairs */
        float s[64];
        double d[32];
        uint32_t su[64];
    };
    uint32_t excl_addr;              /* exclusive monitor (ldrex/strex) */
    uint32_t excl_size;
    uint64_t excl_val;
    volatile uint32_t sigpending;    /* guest signals waiting for a safe point */
    uint32_t tls;                    /* guest TLS block (bionic layout) */
    void *thread;                    /* runtime per-thread data */
} CPU;

typedef void (*GuestFnPtr)(CPU *c);
typedef struct { uint32_t vaddr; GuestFnPtr fn; } GuestFn;
typedef struct { uint32_t vaddr; const char *name; } GuestSym;

/* ---- memory (memcpy keeps unaligned accesses legal on every host) */
extern uint8_t *g_mem;
#define GP(a) (g_mem + (uint32_t)(a))
static inline uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, (void *)GP(a), 4); return v; }
static inline uint32_t R16(uint32_t a) { uint16_t v; memcpy(&v, (void *)GP(a), 2); return v; }
static inline uint32_t R8(uint32_t a) { return *(uint8_t *)GP(a); }
static inline uint32_t RS16(uint32_t a) { int16_t v; memcpy(&v, (void *)GP(a), 2); return (uint32_t)(int32_t)v; }
static inline uint32_t RS8(uint32_t a) { return (uint32_t)(int32_t)*(int8_t *)GP(a); }
static inline void W32(uint32_t a, uint32_t v) { memcpy((void *)GP(a), &v, 4); }
static inline void W16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy((void *)GP(a), &x, 2); }
static inline void W8(uint32_t a, uint32_t v) { *(uint8_t *)GP(a) = (uint8_t)v; }
static inline float RVF(uint32_t a) { float v; memcpy(&v, (void *)GP(a), 4); return v; }
static inline double RV64(uint32_t a) { double v; memcpy(&v, (void *)GP(a), 8); return v; }
static inline void WVF(uint32_t a, float v) { memcpy((void *)GP(a), &v, 4); }
static inline void WV64(uint32_t a, double v) { memcpy((void *)GP(a), &v, 8); }

/* ---- exclusive access: real atomics between host threads (compare-and-swap against the loaded value) */
static inline uint32_t LDREX(CPU *c, uint32_t a, uint32_t size) {
    uint32_t v;
    if (size == 4) v = __atomic_load_n((uint32_t *)GP(a), __ATOMIC_SEQ_CST);
    else if (size == 2) v = __atomic_load_n((uint16_t *)GP(a), __ATOMIC_SEQ_CST);
    else v = __atomic_load_n((uint8_t *)GP(a), __ATOMIC_SEQ_CST);
    c->excl_addr = a; c->excl_size = size; c->excl_val = v;
    return v;
}
static inline uint32_t STREX(CPU *c, uint32_t a, uint32_t v, uint32_t size) {
    int ok = 0;
    if (c->excl_addr == a && c->excl_size == size) {
        if (size == 4) { uint32_t e = (uint32_t)c->excl_val; ok = __atomic_compare_exchange_n((uint32_t *)GP(a), &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
        else if (size == 2) { uint16_t e = (uint16_t)c->excl_val; ok = __atomic_compare_exchange_n((uint16_t *)GP(a), &e, (uint16_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
        else { uint8_t e = (uint8_t)c->excl_val; ok = __atomic_compare_exchange_n((uint8_t *)GP(a), &e, (uint8_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    }
    c->excl_addr = 0xffffffffu;
    return ok ? 0u : 1u;
}
static inline uint64_t LDREXD(CPU *c, uint32_t a) {
    uint64_t v = __atomic_load_n((uint64_t *)GP(a), __ATOMIC_SEQ_CST);
    c->excl_addr = a; c->excl_size = 8; c->excl_val = v;
    return v;
}
static inline uint32_t STREXD(CPU *c, uint32_t a, uint64_t v) {
    int ok = 0;
    if (c->excl_addr == a && c->excl_size == 8) {
        uint64_t e = c->excl_val;
        ok = __atomic_compare_exchange_n((uint64_t *)GP(a), &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    c->excl_addr = 0xffffffffu;
    return ok ? 0u : 1u;
}
static inline uint32_t SWP(CPU *c, uint32_t a, uint32_t v, uint32_t size) {
    (void)c;
    if (size == 1) return __atomic_exchange_n((uint8_t *)GP(a), (uint8_t)v, __ATOMIC_SEQ_CST);
    return __atomic_exchange_n((uint32_t *)GP(a), v, __ATOMIC_SEQ_CST);
}

/* ---- flags */
static inline uint32_t addc(CPU *c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t r = (uint64_t)a + b + cin; uint32_t res = (uint32_t)r;
    c->n = res >> 31; c->z = res == 0; c->cf = (uint32_t)(r >> 32);
    c->v = ((~(a ^ b)) & (a ^ res)) >> 31;
    return res;
}

/* ---- shifter: kind 0 lsl 1 lsr 2 asr 3 ror 4 rrx */
static inline uint32_t ror32(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline uint32_t sh(uint32_t v, int kind, uint32_t n, uint32_t cin) {
    switch (kind) {
    case 0: return n >= 32 ? 0 : v << n;
    case 1: return n >= 32 ? 0 : v >> n;
    case 2: return n >= 32 ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> n);
    case 3: return ror32(v, n);
    default: return (v >> 1) | (cin << 31);
    }
}
static inline uint32_t shc(CPU *c, uint32_t v, int kind, uint32_t n, uint32_t *carry) {
    *carry = c->cf;
    if (kind == 4) { *carry = v & 1; return (v >> 1) | (c->cf << 31); }
    if (n == 0) return v;
    switch (kind) {
    case 0: if (n < 32) { *carry = (v >> (32 - n)) & 1; return v << n; }
            *carry = n == 32 ? (v & 1) : 0; return 0;
    case 1: if (n < 32) { *carry = (v >> (n - 1)) & 1; return v >> n; }
            *carry = n == 32 ? (v >> 31) : 0; return 0;
    case 2: if (n < 32) { *carry = (v >> (n - 1)) & 1; return (uint32_t)((int32_t)v >> n); }
            *carry = v >> 31; return (uint32_t)((int32_t)v >> 31);
    default: { uint32_t r = ror32(v, n); *carry = r >> 31; return r; }
    }
}

static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
static inline uint32_t rbit32(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4);
    return __builtin_bswap32(v);
}
static inline uint32_t usat(uint32_t v, int bits) {
    int32_t x = (int32_t)v; int32_t mx = (int32_t)((1u << bits) - 1);
    return (uint32_t)(x < 0 ? 0 : x > mx ? mx : x);
}
static inline uint32_t ssat(uint32_t v, int bits) {
    int32_t x = (int32_t)v; int32_t mx = (1 << (bits - 1)) - 1, mn = -(1 << (bits - 1));
    return (uint32_t)(x < mn ? mn : x > mx ? mx : x);
}

/* ---- VFP */
static inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float fromb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double fromd(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
/* x86 produces a negative "indefinite" NaN where ARM produces the default NaN 0x7fc00000 */
static inline float fnan(float x) { if (x != x) { uint32_t u; memcpy(&u, &x, 4); u &= 0x7fffffffu; memcpy(&x, &u, 4); } return x; }
static inline double dnan(double x) { if (x != x) { uint64_t u; memcpy(&u, &x, 8); u &= 0x7fffffffffffffffull; memcpy(&x, &u, 8); } return x; }
static inline uint32_t f2s(double x) {          /* VCVT.S32: round toward zero, saturate, NaN -> 0 */
    if (x != x) return 0;
    if (x >= 2147483647.0) return 0x7fffffffu;
    if (x <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)x;
}
static inline uint32_t f2u(double x) {
    if (x != x || x <= 0.0) return 0;
    if (x >= 4294967295.0) return 0xffffffffu;
    return (uint32_t)x;
}
static inline uint32_t f2sr(double x) { return f2s(nearbyint(x)); }
static inline uint32_t f2ur(double x) { return f2u(nearbyint(x)); }
static inline void fcmp(CPU *c, double a, double b) {    /* sets FPSCR NZCV */
    uint32_t f;
    if (a != a || b != b) f = 0x3u;        /* unordered: C V */
    else if (a == b) f = 0x6u;             /* Z C */
    else if (a < b) f = 0x8u;              /* N */
    else f = 0x2u;                         /* C */
    c->fpscr = (c->fpscr & 0x0fffffffu) | (f << 28);
}

/* ---- runtime services used by generated code */
void call_addr(CPU *c, uint32_t addr);                   /* indirect call (interprets unknown code) */
void jump_addr(CPU *c, uint32_t addr, uint32_t elr);    /* indirect jump out of a function body: ends at elr */
void unimpl(CPU *c, uint32_t elr, uint32_t addr, const char *what);   /* continue in the interpreter */
void guest_svc(CPU *c);                                  /* svc #0: Linux syscall */
uint32_t guest_tls(CPU *c);                              /* TPIDRURO */
void guest_poll(CPU *c);                                 /* deliver pending guest signals */

/* indirect jump: a jump to our own return address is a return */
#define JUMP(x) do { uint32_t _j = (x); if ((_j | 1u) == elr) return; jump_addr(c, _j, elr); return; } while (0)

/* debug builds record the guest PC of every instruction in r15 (crash reports) */
#if defined(RB_TRACE)
void rb_trace(CPU *c);
#define PC(a) (c->r[15] = (a), rb_trace(c))
#elif defined(RB_DEBUG)
#define PC(a) (c->r[15] = (a))
#else
#define PC(a) ((void)0)
#endif

/* setjmp/sigsetjmp called by guest code: must run in the guest function's own frame */
#if defined(__x86_64__) && defined(_WIN32)
typedef void *rb_jmp_buf[5];
#define RB_SETJMP(b) __builtin_setjmp(b)
#define RB_LONGJMP(b) __builtin_longjmp(b, 1)
#elif !defined(_WIN32)
#include <setjmp.h>
typedef jmp_buf rb_jmp_buf;
#define RB_SETJMP(b) _setjmp(b)          /* no signal mask save: cheap */
#define RB_LONGJMP(b) _longjmp(b, 1)
#else
#include <setjmp.h>
typedef jmp_buf rb_jmp_buf;
#define RB_SETJMP(b) setjmp(b)
#define RB_LONGJMP(b) longjmp(b, 1)
#endif
rb_jmp_buf *guest_jmpbuf_for(CPU *c, uint32_t guest_buf);
void guest_jmp_saved(CPU *c, uint32_t guest_buf);
uint32_t guest_jmp_value(CPU *c, uint32_t guest_buf);
#define GUEST_SETJMP(c) do { uint32_t _gb = (c)->r[0]; guest_jmp_saved((c), _gb); \
        (c)->r[0] = RB_SETJMP(*guest_jmpbuf_for((c), _gb)) ? guest_jmp_value((c), _gb) : 0u; } while (0)

#ifdef __cplusplus
}
#endif
