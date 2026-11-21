// Core runtime: guest memory, heap, loader (libunity + libmono), dispatch, guest calls, setjmp.
#include "rt.h"
#include "host.h"
#include "elf32.h"
#include "thread.h"
#include "profile.h"
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <algorithm>
#ifdef __ANDROID__
#include <android/log.h>
#else
#include <execinfo.h>
#endif

extern "C" {
typedef void *mspace;
mspace create_mspace_with_base(void *base, size_t capacity, int locked);
void *mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void *mem);
void *mspace_realloc(mspace msp, void *mem, size_t newsize);
void *mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);
void *mspace_memalign(mspace msp, size_t alignment, size_t bytes);
size_t mspace_usable_size(const void *mem);

extern const GuestFn U_fns[]; extern const unsigned U_fn_count;
extern const GuestSym U_syms[]; extern const unsigned U_sym_count;
extern const GuestFn M_fns[]; extern const unsigned M_fn_count;
extern const GuestSym M_syms[]; extern const unsigned M_sym_count;
extern const char *const import_names[];        // generated: every import the libraries need
extern const unsigned import_count;
}
uint8_t *g_mem;

namespace rt {
std::string data_dir, apk_path, cache_dir;
int verbose = 0;
static mspace heap;

void fatal(const char *fmt, ...) {
    char msg[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    fprintf(stderr, "[reborn2droid] FATAL: %s\n", msg); fflush(stderr);
#ifndef __ANDROID__
    { void *bt[40]; int n = backtrace(bt, 40); backtrace_symbols_fd(bt, n, 2); }
#endif
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_FATAL, "Reborn2Droid", "FATAL: %s", msg);
#endif
    host::show_error(RB_LABEL, msg);
    abort();
}
void logf(const char *fmt, ...) {
    char msg[4096];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "Reborn2Droid", "%s", msg);
#else
    fprintf(stderr, "[reborn2droid] %s\n", msg);
#endif
}

static void map_fixed(uint32_t addr, size_t len) {
    if (!host::commit(GPTR(addr), len)) fatal("cannot commit guest memory at 0x%08x (%zu bytes)", addr, len);
}

extern thread_local Thread *t_self_public;
static void on_crash(const char *what, void *addr) {
    CPU *c = nullptr;
    if (Thread *t = t_self_public) c = t->c;
    long long ga = (uint8_t *)addr >= g_mem && (uint8_t *)addr < g_mem + 0x100000000ull ? (long long)((uint8_t *)addr - g_mem) : -1;
    logf("CRASH %s at host addr %p (guest 0x%llx)", what, addr, ga);
    if (c) {
        logf("guest pc=0x%08x (%s)", c->r[15], sym_name(c->r[15]));
        logf("guest lr=0x%08x (%s)", c->r[14], sym_name(c->r[14]));
        char line[256]; int n = 0;
        for (int k = 0; k < 16; k++) { n += snprintf(line + n, sizeof line - n, " r%-2d=%08x", k, c->r[k]); if (k % 4 == 3) { logf("%s", line); n = 0; } }
        uint32_t sp = c->r[13];
        for (int k = 0; k < 24; k++) { uint32_t v = R32(sp + 4 * k); if (is_static_code(v & ~1u)) logf("  stack+%02x: %s", 4 * k, sym_name(v)); }
    }
    fflush(stderr);
}

void mem_init() {
    if (g_mem) return;
    g_mem = (uint8_t *)host::reserve(0x100000000ull + 0x10000);
    if (!g_mem) fatal("cannot reserve 4 GB of address space for the game");
    host::install_crash_handler(on_crash);
    map_fixed(HEAP_LO, HEAP_HI - HEAP_LO);
    map_fixed(0xffff0000u, 0x10000);                     // kernel helper page: version word at 0xffff0ffc
    W32(0xffff0ffcu, 3);
    heap = create_mspace_with_base(GPTR(HEAP_LO), HEAP_HI - HEAP_LO, 1);
    if (!heap) fatal("heap");
}
uint32_t gmalloc(size_t n) { void *p = mspace_malloc(heap, n ? n : 1); return p ? G32(p) : 0; }
uint32_t gcalloc(size_t n, size_t m) { void *p = mspace_calloc(heap, n, m); return p ? G32(p) : 0; }
uint32_t grealloc(uint32_t p, size_t n) { void *r = mspace_realloc(heap, p ? GPTR(p) : nullptr, n); return r ? G32(r) : 0; }
uint32_t gmemalign(size_t a, size_t n) { void *p = mspace_memalign(heap, a, n ? n : 1); return p ? G32(p) : 0; }
size_t gmalloc_usable(uint32_t p) { return p ? mspace_usable_size(GPTR(p)) : 0; }
void gfree(uint32_t p) { if (p) mspace_free(heap, GPTR(p)); }
uint32_t gstrdup(const char *s) { size_t n = strlen(s) + 1; uint32_t p = gmalloc(n); memcpy(GPTR(p), s, n); return p; }
uint32_t gstatic(size_t n) { uint32_t p = gmalloc(n); memset(GPTR(p), 0, n); return p; }

// ---------------------------------------------------------------- libraries
struct Lib {
    std::string name;
    uint32_t base = 0, size = 0, text_lo = 0, text_hi = 0;
    const GuestFn *fns = nullptr; unsigned nfns = 0;
    const GuestSym *syms = nullptr; unsigned nsyms = 0;
    std::vector<uint8_t> elf;
    std::unordered_map<std::string, uint32_t> exports;
    uint32_t init_array = 0, init_array_sz = 0, init = 0;
};
static std::vector<Lib *> libs;

static Lib *lib_of(uint32_t addr) {
    for (Lib *l : libs) if (addr - l->base < l->size) return l;
    return nullptr;
}
bool is_static_code(uint32_t addr) {
    for (Lib *l : libs) if (addr - l->base - l->text_lo < l->text_hi - l->text_lo) return true;
    return false;
}

static GuestFnPtr find_in(Lib *l, uint32_t vaddr) {
    unsigned lo = 0, hi = l->nfns;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (l->fns[mid].vaddr < vaddr) lo = mid + 1; else hi = mid;
    }
    if (lo < l->nfns && l->fns[lo].vaddr == vaddr) return l->fns[lo].fn;
    return nullptr;
}

static const uint32_t DYN_TRAP = TRAP_BASE + 0x100000u;
static std::vector<GuestFnPtr> dyn_traps;
static std::vector<std::string> dyn_trap_names;
const char *sym_name(uint32_t addr) {
    static thread_local char buf[512];
    Lib *l = lib_of(addr & ~1u);
    if (!l && addr >= DYN_TRAP && addr < TRAP_END) {
        uint32_t k = (addr - DYN_TRAP) / 4;
        snprintf(buf, sizeof buf, "trap:%s", k < dyn_trap_names.size() ? dyn_trap_names[k].c_str() : "?"); return buf;
    }
    if (!l && addr >= TRAP_BASE && addr < DYN_TRAP) {
        uint32_t k = (addr - TRAP_BASE) / 4;
        snprintf(buf, sizeof buf, "import:%s", k < import_count ? import_names[k] : "?"); return buf;
    }
    if (!l) { snprintf(buf, sizeof buf, "0x%08x", addr); return buf; }
    uint32_t v = (addr - l->base) & ~1u; const char *best = nullptr; uint32_t bv = 0;
    for (unsigned k = 0; k < l->nsyms; k++)
        if (l->syms[k].vaddr <= v && l->syms[k].vaddr >= bv) { bv = l->syms[k].vaddr; best = l->syms[k].name; }
    if (best && v - bv < 0x4000) snprintf(buf, sizeof buf, "%s:%s+0x%x", l->name.c_str(), best, v - bv);
    else snprintf(buf, sizeof buf, "%s+0x%x", l->name.c_str(), v);
    return buf;
}

// ---- traps: imports and runtime-created entry points
static std::map<std::string, GuestFnPtr> &imp_registry() { static std::map<std::string, GuestFnPtr> m; return m; }
ImpReg::ImpReg(const char *name, GuestFnPtr fn) { imp_registry()[name] = fn; }
static std::vector<GuestFnPtr> imp_fns;
static std::mutex trap_mu;

extern "C" void missing_import(CPU *c, const char *name);
static void resolve_imports() {
    if (!imp_fns.empty()) return;
    imp_fns.resize(import_count);
    for (unsigned k = 0; k < import_count; k++) {
        auto it = imp_registry().find(import_names[k]);
        imp_fns[k] = it == imp_registry().end() ? nullptr : it->second;
    }
}
uint32_t import_trap(const char *name) {
    resolve_imports();
    for (unsigned k = 0; k < import_count; k++)
        if (!strcmp(import_names[k], name)) return TRAP_BASE + 4 * k;
    // imports reached only through dlsym (not in any PLT): give them a dynamic trap if implemented
    auto it = imp_registry().find(name);
    if (it != imp_registry().end()) return new_trap(it->second, name);
    return 0;
}
uint32_t new_trap(GuestFnPtr fn, const char *name) {
    std::lock_guard<std::mutex> g(trap_mu);
    for (size_t k = 0; k < dyn_traps.size(); k++)
        if (dyn_traps[k] == fn && dyn_trap_names[k] == name) return DYN_TRAP + 4 * (uint32_t)k;
    dyn_traps.push_back(fn); dyn_trap_names.push_back(name);
    return DYN_TRAP + 4 * (uint32_t)(dyn_traps.size() - 1);
}

static thread_local const char *t_missing;
static void call_missing(CPU *c) { missing_import(c, t_missing); }

// ---- ARM Linux kernel user helpers (0xffff0f60..0xffff0fe0)
static void kuser_cmpxchg(CPU *c) {           // r0 oldval, r1 newval, r2 ptr -> r0 = 0 and C set on success
    uint32_t expect = c->r[0];
    bool ok = __atomic_compare_exchange_n((uint32_t *)GPTR(c->r[2]), &expect, c->r[1], false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    c->r[0] = ok ? 0 : 1; c->cf = ok ? 1 : 0; c->z = ok ? 1 : 0; c->n = 0;
}
static void kuser_cmpxchg64(CPU *c) {         // r0 *old, r1 *new, r2 ptr
    uint64_t expect = (uint64_t)R32(c->r[0]) | ((uint64_t)R32(c->r[0] + 4) << 32);
    uint64_t nv = (uint64_t)R32(c->r[1]) | ((uint64_t)R32(c->r[1] + 4) << 32);
    bool ok = __atomic_compare_exchange_n((uint64_t *)GPTR(c->r[2]), &expect, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    c->r[0] = ok ? 0 : 1; c->cf = ok ? 1 : 0; c->z = ok ? 1 : 0;
}
static void kuser_barrier(CPU *) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static void kuser_get_tls(CPU *c) { c->r[0] = c->tls; }

GuestFnPtr host_fn(uint32_t addr) {
    if (__builtin_expect(addr >= 0xffff0f60u, 0)) {
        switch (addr & ~1u) {
        case 0xffff0fc0: return kuser_cmpxchg;
        case 0xffff0fa0: return kuser_barrier;
        case 0xffff0fe0: return kuser_get_tls;
        case 0xffff0f60: return kuser_cmpxchg64;
        }
        return nullptr;
    }
    if (addr - TRAP_BASE < TRAP_END - TRAP_BASE) {
        if (addr >= DYN_TRAP) {
            uint32_t k = (addr - DYN_TRAP) / 4;
            std::lock_guard<std::mutex> g(trap_mu);
            return k < dyn_traps.size() ? dyn_traps[k] : nullptr;
        }
        uint32_t k = (addr - TRAP_BASE) / 4;
        if (k >= import_count) return nullptr;
        if (!imp_fns[k]) { t_missing = import_names[k]; return call_missing; }
        return imp_fns[k];
    }
    Lib *l = lib_of(addr & ~1u);
    if (!l) return nullptr;
    return find_in(l, (addr - l->base) & ~1u);
}

}  // namespace rt
using namespace rt;

// static code the interpreter had to run: RB_ENTRIES=<file> collects them as extra recompiler roots
static std::mutex entries_mu;
static std::map<uint32_t, std::string> entries;
static void note_static_entry(uint32_t addr, const char *why) {
    std::lock_guard<std::mutex> g(entries_mu);
    if (entries.emplace(addr, why).second && verbose) logf("interpreting library code at %s (%s)", sym_name(addr), why);
}
void dump_static_entries() {
    const char *f = getenv("RB_ENTRIES"); if (!f) return;
    std::lock_guard<std::mutex> g(entries_mu);
    FILE *o = fopen(f, "a"); if (!o) return;
    for (auto &kv : entries) fprintf(o, "%08x %s %s\n", kv.first, kv.second.c_str(), sym_name(kv.first));
    fclose(o);
}
extern "C" void call_addr(CPU *c, uint32_t addr) {
    GuestFnPtr f = host_fn(addr);
    if (f) { f(c); return; }
    if (addr < NULL_GUARD)
        fatal("call to null guest address 0x%08x, lr=0x%08x (%s)", addr, c->r[14], sym_name(c->r[14]));
    if (is_static_code(addr)) note_static_entry(addr, "call");
    interp::call(c, addr);
}

extern "C" void unimpl(CPU *c, uint32_t elr, uint32_t addr, const char *what) {
    note_static_entry(addr, what);
    interp::resume(c, addr, elr);
}

extern "C" void missing_import(CPU *c, const char *name) {
    fatal("import not implemented: %s (called from %s)", name, sym_name(c->r[14]));
}

namespace rt {
uint32_t arg(CPU *c, int i) { return i < 4 ? c->r[i] : R32(c->r[13] + 4 * (i - 4)); }
uint64_t arg64(CPU *c, int &i) {
    if (i < 4 && (i & 1)) i++;
    uint32_t lo, hi;
    if (i + 1 < 4) { lo = c->r[i]; hi = c->r[i + 1]; i += 2; }
    else {
        if (i < 4) i = 4;                                     // 64-bit values are never split
        int so = (i - 4) * 4; if (so & 7) { so += 4; i++; }   // 8-byte aligned on the stack
        lo = R32(c->r[13] + so); hi = R32(c->r[13] + so + 4); i += 2;
    }
    return ((uint64_t)hi << 32) | lo;
}
void ret64(CPU *c, uint64_t v) { c->r[0] = (uint32_t)v; c->r[1] = (uint32_t)(v >> 32); }

static void host_return_fn(CPU *) {}
uint32_t host_return_addr() { static uint32_t a = new_trap(host_return_fn, "__return_to_host"); return a; }
static const uint32_t RET_TRAP_NAME = 0;   // (unused)
uint32_t call_c(CPU *c, uint32_t addr, std::initializer_list<uint32_t> args) {
    uint32_t saved_sp = c->r[13], saved_lr = c->r[14], saved_r[4] = {c->r[0], c->r[1], c->r[2], c->r[3]};
    (void)saved_r; (void)RET_TRAP_NAME;
    std::vector<uint32_t> a(args);
    uint32_t sp = (c->r[13] - 16) & ~7u;
    if (a.size() > 4) {
        uint32_t n = (uint32_t)(a.size() - 4);
        sp = (sp - 4 * n) & ~7u;
        for (uint32_t k = 0; k < n; k++) W32(sp + 4 * k, a[4 + k]);
    }
    c->r[13] = sp;
    for (size_t k = 0; k < a.size() && k < 4; k++) c->r[k] = a[k];
    c->r[14] = host_return_addr();                            // a jump here (instead of a host return) is a no-op
    call_addr(c, addr);
    uint32_t r = c->r[0];
    c->r[13] = saved_sp; c->r[14] = saved_lr;
    return r;
}
uint32_t call(uint32_t addr, std::initializer_list<uint32_t> args) { return call_c(cpu(), addr, args); }

// ---------------------------------------------------------------- loader
static std::unordered_map<std::string, uint32_t> special_data;   // data symbols provided by the runtime

extern "C" uint32_t rt_sF();
extern "C" uint32_t rt_data_symbol(const char *name);   // rt_libc: _ctype_, environ, ...

Lib *load_library(const char *path, uint32_t base) {
    resolve_imports();
    Lib *L = new Lib();
    const char *slash = strrchr(path, '/'); L->name = slash ? slash + 1 : path;
    L->base = base;
    FILE *f = host::fopen(path, "rb"); if (!f) fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    L->elf.resize(n); if (fread(L->elf.data(), 1, n, f) != (size_t)n) fatal("read %s", path); fclose(f);
    auto *eh = (Elf32_Ehdr *)L->elf.data();
    auto *ph = (Elf32_Phdr *)(L->elf.data() + eh->e_phoff);
    uint32_t hi = 0;
    for (int k = 0; k < eh->e_phnum; k++)
        if (ph[k].p_type == PT_LOAD) hi = std::max(hi, ph[k].p_vaddr + ph[k].p_memsz);
    L->size = (hi + 0xffff) & ~0xffffu;
    map_fixed(base, L->size);
    Elf32_Dyn *dyn = nullptr;
    for (int k = 0; k < eh->e_phnum; k++) {
        if (ph[k].p_type == PT_LOAD) memcpy(GPTR(base + ph[k].p_vaddr), L->elf.data() + ph[k].p_offset, ph[k].p_filesz);
        if (ph[k].p_type == PT_DYNAMIC) dyn = (Elf32_Dyn *)GPTR(base + ph[k].p_vaddr);
    }
    // text range from section headers
    auto *sh = (Elf32_Shdr *)(L->elf.data() + eh->e_shoff);
    const char *shstr = (const char *)L->elf.data() + sh[eh->e_shstrndx].sh_offset;
    for (int k = 0; k < eh->e_shnum; k++)
        if (!strcmp(shstr + sh[k].sh_name, ".text")) { L->text_lo = sh[k].sh_addr; L->text_hi = sh[k].sh_addr + sh[k].sh_size; }
    if (L->name.find("unity") != std::string::npos) { L->fns = U_fns; L->nfns = U_fn_count; L->syms = U_syms; L->nsyms = U_sym_count; }
    else if (L->name.find("mono") != std::string::npos) { L->fns = M_fns; L->nfns = M_fn_count; L->syms = M_syms; L->nsyms = M_sym_count; }
    else fatal("no recompiled code for %s", path);

    uint32_t rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, symtab = 0, strtab = 0, hash = 0;
    for (Elf32_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_REL: rel = d->d_un.d_ptr; break;
        case DT_RELSZ: relsz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
        case DT_STRTAB: strtab = d->d_un.d_ptr; break;
        case DT_HASH: hash = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: L->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: L->init_array_sz = d->d_un.d_val; break;
        case DT_INIT: L->init = d->d_un.d_ptr; break;
        }
    }
    auto *syms = (Elf32_Sym *)GPTR(base + symtab);
    const char *strs = GSTR(base + strtab);
    uint32_t nsyms = R32(base + hash + 4);
    for (uint32_t k = 1; k < nsyms; k++)
        if (syms[k].st_shndx != SHN_UNDEF && syms[k].st_value) L->exports[strs + syms[k].st_name] = base + syms[k].st_value;
    libs.push_back(L);

    auto resolve = [&](uint32_t symidx) -> uint32_t {
        Elf32_Sym &s = syms[symidx];
        const char *name = strs + s.st_name;
        if (s.st_shndx != SHN_UNDEF) return base + s.st_value;
        for (Lib *o : libs) {                                       // other guest libraries first (libmono for libunity)
            auto it = o->exports.find(name);
            if (it != o->exports.end()) return it->second;
        }
        if (uint32_t d = rt_data_symbol(name)) return d;
        uint32_t t = import_trap(name);
        if (!t) logf("unresolved import %s in %s", name, L->name.c_str());
        return t;
    };
    auto apply = [&](uint32_t start, uint32_t size) {
        for (uint32_t o = 0; o < size; o += 8) {
            auto *r = (Elf32_Rel *)GPTR(base + start + o);
            uint32_t P = base + r->r_offset, type = ELF32_R_TYPE(r->r_info), si = ELF32_R_SYM(r->r_info);
            switch (type) {
            case R_ARM_RELATIVE: W32(P, R32(P) + base); break;
            case R_ARM_ABS32: W32(P, R32(P) + resolve(si)); break;
            case R_ARM_GLOB_DAT: case R_ARM_JUMP_SLOT: W32(P, resolve(si)); break;
            default: fatal("relocation type %u", type);
            }
        }
    };
    apply(rel, relsz);
    apply(jmprel, pltrelsz);
    logf("loaded %s at 0x%08x (%zu exports, %u recompiled functions)", L->name.c_str(), base, L->exports.size(), L->nfns);
    return L;
}

uint32_t lib_sym(Lib *l, const char *name) {
    auto it = l->exports.find(name);
    return it == l->exports.end() ? 0 : it->second;
}
uint32_t sym_addr(const char *name) {
    for (Lib *l : libs) if (uint32_t a = lib_sym(l, name)) return a;
    return 0;
}

void run_initializers(Lib *L) {
    if (L->init) call(L->base + L->init, {});
    for (uint32_t o = 0; o < L->init_array_sz; o += 4) {
        uint32_t fn = R32(L->base + L->init_array + o);
        if (fn && fn != 0xffffffffu) call(fn, {});
    }
    logf("%s: ran %u initializers", L->name.c_str(), L->init_array_sz / 4);
}
}  // namespace rt

// ---------------------------------------------------------------- setjmp / longjmp
struct JmpSlot { rb_jmp_buf jb; uint32_t r[16]; double d[16]; uint32_t val; };
static std::mutex jmp_mu;
static std::unordered_map<uint64_t, JmpSlot *> jmp_slots;   // (thread cpu, guest buffer) -> slot

static JmpSlot *slot_for(CPU *c, uint32_t gb) {
    std::lock_guard<std::mutex> g(jmp_mu);
    uint64_t key = ((uint64_t)(uintptr_t)c << 32) ^ gb;
    auto &s = jmp_slots[key];
    if (!s) s = new JmpSlot();
    return s;
}
extern "C" rb_jmp_buf *guest_jmpbuf_for(CPU *c, uint32_t gb) { return &slot_for(c, gb)->jb; }
extern "C" uint32_t guest_jmp_value(CPU *c, uint32_t gb) { return slot_for(c, gb)->val; }
extern "C" void guest_jmp_saved(CPU *c, uint32_t gb) {
    JmpSlot *s = slot_for(c, gb);
    memcpy(s->r, c->r, sizeof s->r);
    memcpy(s->d, c->d, sizeof s->d);
    W32(gb, 0x41413231u);                                   // mark the guest buffer as used
}
static void do_longjmp(CPU *c, uint32_t gb, uint32_t val) {
    JmpSlot *s = slot_for(c, gb);
    memcpy(c->r, s->r, sizeof s->r);
    memcpy(c->d, s->d, sizeof s->d);
    s->val = val ? val : 1;
    RB_LONGJMP(s->jb);
}
IMP(longjmp) { do_longjmp(c, c->r[0], c->r[1]); }
IMP(siglongjmp) { do_longjmp(c, c->r[0], c->r[1]); }
IMP(_longjmp) { do_longjmp(c, c->r[0], c->r[1]); }
// setjmp reached through a function pointer (not a direct call): cannot return twice safely
IMP(setjmp) { guest_jmp_saved(c, c->r[0]); c->r[0] = 0; }
IMP(sigsetjmp) { guest_jmp_saved(c, c->r[0]); c->r[0] = 0; }

// ---------------------------------------------------------------- native libgcc helpers
extern "C" {
void nat___aeabi_idiv(CPU *c) { int32_t a = c->r[0], b = c->r[1]; c->r[0] = b ? (a == INT32_MIN && b == -1 ? (uint32_t)a : (uint32_t)(a / b)) : 0; }
void nat___divsi3(CPU *c) { nat___aeabi_idiv(c); }
void nat___aeabi_uidiv(CPU *c) { uint32_t a = c->r[0], b = c->r[1]; c->r[0] = b ? a / b : 0; }
void nat___udivsi3(CPU *c) { nat___aeabi_uidiv(c); }
void nat___aeabi_idivmod(CPU *c) {
    int32_t a = c->r[0], b = c->r[1];
    if (!b) { c->r[0] = 0; c->r[1] = 0; return; }
    if (a == INT32_MIN && b == -1) { c->r[0] = a; c->r[1] = 0; return; }
    c->r[0] = (uint32_t)(a / b); c->r[1] = (uint32_t)(a % b);
}
void nat___aeabi_uidivmod(CPU *c) {
    uint32_t a = c->r[0], b = c->r[1];
    if (!b) { c->r[0] = 0; c->r[1] = 0; return; }
    c->r[0] = a / b; c->r[1] = a % b;
}
void nat___aeabi_ldivmod(CPU *c) {
    int64_t a = (int64_t)(((uint64_t)c->r[1] << 32) | c->r[0]), b = (int64_t)(((uint64_t)c->r[3] << 32) | c->r[2]);
    int64_t q = 0, r = 0;
    if (b == -1 && a == INT64_MIN) { q = a; r = 0; } else if (b) { q = a / b; r = a % b; }
    c->r[0] = (uint32_t)q; c->r[1] = (uint32_t)((uint64_t)q >> 32); c->r[2] = (uint32_t)r; c->r[3] = (uint32_t)((uint64_t)r >> 32);
}
void nat___aeabi_uldivmod(CPU *c) {
    uint64_t a = ((uint64_t)c->r[1] << 32) | c->r[0], b = ((uint64_t)c->r[3] << 32) | c->r[2];
    uint64_t q = b ? a / b : 0, r = b ? a % b : 0;
    c->r[0] = (uint32_t)q; c->r[1] = (uint32_t)(q >> 32); c->r[2] = (uint32_t)r; c->r[3] = (uint32_t)(r >> 32);
}
void nat___aeabi_idiv0(CPU *c) { c->r[0] = 0; }
void nat___aeabi_ldiv0(CPU *c) { c->r[0] = 0; c->r[1] = 0; }
}

// imports implemented by another guest library (libunity calling libmono): resolved once, then cached
extern "C" void dyn_import(CPU *c, unsigned idx) {
    static uint32_t *cache;
    static std::once_flag once;
    std::call_once(once, [] { cache = (uint32_t *)calloc(import_count, 4); });
    uint32_t a = __atomic_load_n(&cache[idx], __ATOMIC_RELAXED);
    if (!a) {
        a = sym_addr(import_names[idx]);
        if (!a) missing_import(c, import_names[idx]);
        __atomic_store_n(&cache[idx], a, __ATOMIC_RELAXED);
    }
    call_addr(c, a);
}
