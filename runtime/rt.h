// Internal runtime API (C++). The recompiled code only sees guest.h.
#pragma once
#include "guest.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <initializer_list>

#define GPTR(x) ((void *)GP(x))
#define GSTR(x) ((const char *)GP(x))
static inline uint32_t G32(const void *p) { return (uint32_t)((const uint8_t *)p - g_mem); }   // host ptr inside guest memory -> guest addr

namespace rt {
// ---- guest address map
constexpr uint32_t NULL_GUARD = 0x00010000u;      // below: never mapped (null page faults)
constexpr uint32_t TRAP_BASE = 0x0F000000u, TRAP_END = 0x0F400000u;   // import / JNI / special trap addresses
constexpr uint32_t UNITY_BASE = 0x10000000u, MONO_BASE = 0x11000000u;
constexpr uint32_t HEAP_LO = 0x12000000u, HEAP_HI = 0xF0000000u;

// ---- memory
void mem_init();
uint32_t gmalloc(size_t n);
uint32_t gcalloc(size_t n, size_t m);
uint32_t grealloc(uint32_t p, size_t n);
uint32_t gmemalign(size_t align, size_t n);
size_t gmalloc_usable(uint32_t p);
void gfree(uint32_t p);
uint32_t gstrdup(const char *s);
uint32_t gstatic(size_t n);                // zeroed, never freed

// ---- threads / cpu
struct Thread;
CPU *cpu();                                // CPU of the calling host thread (created on demand)
CPU *new_cpu(size_t stack_bytes);
extern thread_local int *guest_errno;      // guest-visible errno of this thread
void gerr(int e);
void set_errno_from_host();

// ---- calling guest code
uint32_t call(uint32_t addr, std::initializer_list<uint32_t> args);   // r0..r3 + stack
uint32_t call_c(CPU *c, uint32_t addr, std::initializer_list<uint32_t> args);
uint32_t arg(CPU *c, int i);               // i-th 32-bit argument of an import call
uint64_t arg64(CPU *c, int &i);            // 64-bit argument starting at index i (aligned), advances i
void ret64(CPU *c, uint64_t v);
static inline double argd(CPU *c, int &i) { uint64_t u = arg64(c, i); double d; memcpy(&d, &u, 8); return d; }
static inline void retd(CPU *c, double d) { uint64_t u; memcpy(&u, &d, 8); ret64(c, u); }
static inline float argf(CPU *c, int i) { uint32_t u = arg(c, i); float f; memcpy(&f, &u, 4); return f; }
static inline void retf(CPU *c, float f) { uint32_t u; memcpy(&u, &f, 4); c->r[0] = u; }

// ---- loader / symbols / dispatch
struct Lib;
Lib *load_library(const char *path, uint32_t base);
void run_initializers(Lib *l);
uint32_t sym_addr(const char *name);       // exported guest symbol (any loaded library) -> guest address
uint32_t lib_sym(Lib *l, const char *name);
const char *sym_name(uint32_t addr);       // nearest guest symbol (diagnostics)
uint32_t import_trap(const char *name);    // trap address for an imported function, 0 if unknown
bool is_static_code(uint32_t addr);        // inside a loaded library's text
GuestFnPtr host_fn(uint32_t addr);         // recompiled function / import / trap at exactly addr
uint32_t new_trap(GuestFnPtr fn, const char *name);   // a guest-callable address that runs fn
uint32_t host_return_addr();               // lr for host -> guest calls

// ---- guest varargs (printf family)
struct VA {
    CPU *c; int idx; uint32_t ap; bool mem;  // either argument index (regs+stack) or va_list pointer
    uint32_t next32();
    uint64_t next64();
};
std::string gformat(const char *fmt, VA &va);

// ---- import registry: every IMP(name) registers imp_name under "name"
struct ImpReg { ImpReg(const char *name, GuestFnPtr fn); };
#define IMP(name) extern "C" void imp_##name(CPU *c); static rt::ImpReg reg_imp_##name(#name, imp_##name); \
                  extern "C" void imp_##name(CPU *c)

// ---- signals (guest view)
void deliver_fault(CPU *c, uint32_t pc, uint32_t addr);   // synchronous SIGSEGV from interpreted code
void signal_thread(Thread *t, int sig);

// ---- paths
std::string host_path(const char *guest_path);   // map Android paths to host files
extern std::string data_dir, apk_path, cache_dir;

[[noreturn]] void fatal(const char *fmt, ...);
void logf(const char *fmt, ...);
extern int verbose;
}  // namespace rt

namespace interp {
void call(CPU *c, uint32_t pc);                     // run guest code at pc until it returns to c->r[14]
void resume(CPU *c, uint32_t pc, uint32_t stop);   // continue mid-function until a jump to stop
uint64_t steps();
void prof_dump();
}
