// pthreads, synchronization, TLS keys and guest signals.
//
// Guest sync objects (bionic: 4-byte mutex/cond/sem words) hold a tagged handle to a host object.
// Guest signals are delivered at safe points: function entries of recompiled code (c->sigpending),
// the interpreter loop, and every blocking wait below.  A handler runs on the guest stack below the
// interrupted sp with a Linux/ARM ucontext, exactly like the kernel would build it, so Boehm GC's
// suspend handler sees the saved registers on the stack it scans.
#include "thread.h"
#include "host.h"
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <map>
#include <deque>
#include <csetjmp>

using namespace rt;
#define A(i) arg(c, i)
#define RET(v) (c->r[0] = (uint32_t)(v))

enum { G_EPERM = 1, G_ESRCH = 3, G_EINTR = 4, G_EAGAIN = 11, G_EBUSY = 16, G_EINVAL = 22, G_EDEADLK = 35, G_ETIMEDOUT = 110 };
enum { G_SIGSEGV = 11, G_SIGBUS = 7 };

namespace rt {
static std::mutex threads_mu;
static std::unordered_map<uint32_t, Thread *> threads_by_self;
static std::atomic<uint32_t> next_tid{2000};
static thread_local Thread *t_self;
static uint32_t g_main_stack_top;
thread_local int *guest_errno;

static Thread *make_thread(size_t guest_stack) {
    Thread *t = new Thread();
    t->c = (CPU *)calloc(1, sizeof(CPU));
    t->c->thread = t;
    t->c->excl_addr = 0xffffffffu;
    if (guest_stack) {
        t->stack_lo = gmemalign(16, guest_stack);
        if (!t->stack_lo) fatal("out of guest memory for a thread stack");
        t->stack_hi = t->stack_lo + (uint32_t)guest_stack;
        t->c->r[13] = (t->stack_hi - 64) & ~15u;
    }
    t->self = gstatic(256);
    W32(t->self, t->self);
    t->tid = next_tid++;
    t->errno_addr = gstatic(4);
    // bionic-style TLS block: [0] self, [1] thread id, [2] errno pointer
    t->c->tls = gstatic(64 * 4);
    W32(t->c->tls, t->c->tls); W32(t->c->tls + 4, t->self); W32(t->c->tls + 8, t->errno_addr);
    std::lock_guard<std::mutex> g(threads_mu);
    threads_by_self[t->self] = t;
    return t;
}
thread_local Thread *t_self_public;
static void attach(Thread *t) {
    t_self = t; t_self_public = t;
    guest_errno = (int *)GPTR(t->errno_addr);
}
Thread *self_thread() {
    if (!t_self) attach(make_thread(1u << 20));
    return t_self;
}
Thread *main_thread_create(size_t guest_stack) {
    Thread *t = make_thread(guest_stack);
    t->is_main = true;
    g_main_stack_top = t->stack_hi;
    attach(t);
    return t;
}
CPU *cpu() { return self_thread()->c; }
Thread *thread_by_guest(uint32_t s) {
    std::lock_guard<std::mutex> g(threads_mu);
    auto it = threads_by_self.find(s);
    return it == threads_by_self.end() ? nullptr : it->second;
}
void gerr(int e) { self_thread(); *guest_errno = e; }
}  // namespace rt

extern "C" uint32_t guest_tls(CPU *c) { return c->tls; }

// ======================================================================= signals
namespace {
struct SigAct { uint32_t handler = 0, flags = 0; uint64_t mask = 0; };
SigAct sigacts[65];
std::mutex sig_mu;
const uint32_t SA_SIGINFO_ = 4, SA_NODEFER_ = 0x40000000u, SA_RESETHAND_ = 0x80000000u;
uint32_t sigreturn_trap;

void sigreturn_fn(CPU *) {}   // returning to this address ends the handler (never really executed)

uint64_t read_sigset(uint32_t p) { return p ? (uint64_t)R32(p) | ((uint64_t)R32(p + 4) << 32) : 0; }
void write_sigset(uint32_t p, uint64_t v) { if (p) { W32(p, (uint32_t)v); W32(p + 4, (uint32_t)(v >> 32)); } }
inline uint64_t bit(int sig) { return 1ull << (sig - 1); }   // Linux sigset_t: signal n is bit n-1

// run the guest handler for sig on the current thread; ctx_pc = interrupted guest pc; fault_addr for faults
// returns true if the handler changed the resume pc (c->r[15] holds it)
bool run_handler(Thread *t, int sig, uint32_t ctx_pc, uint32_t fault_addr) {
    CPU *c = t->c;
    SigAct sa;
    { std::lock_guard<std::mutex> g(sig_mu); sa = sigacts[sig]; if (sa.flags & SA_RESETHAND_) sigacts[sig] = SigAct(); }
    if (sa.handler == 0 || sa.handler == 1) {                       // SIG_DFL / SIG_IGN
        if (sa.handler == 0 && (sig == G_SIGSEGV || sig == G_SIGBUS || sig == 6 || sig == 8 || sig == 4))
            fatal("guest signal %d at %s (fault addr 0x%08x), no handler", sig, sym_name(ctx_pc), fault_addr);
        return false;
    }
    CPU saved = *c;
    uint32_t sp = (c->r[13] - 0x400) & ~15u;       // frame: ucontext + siginfo (below the red zone)
    uint32_t uc = sp, info = sp + 0x300;
    memset(GPTR(uc), 0, 0x380);
    // ucontext: flags, link, stack(3), sigcontext{trap_no,error_code,oldmask,r0..r15,cpsr,fault_address}, sigmask
    uint32_t sc = uc + 20;
    W32(sc + 8, (uint32_t)t->sigmask);
    for (int k = 0; k < 15; k++) W32(sc + 12 + 4 * k, c->r[k]);
    W32(sc + 12 + 60, ctx_pc);
    uint32_t cpsr = (c->n << 31) | (c->z << 30) | (c->cf << 29) | (c->v << 28) | 0x10;
    W32(sc + 12 + 64, cpsr);
    W32(sc + 12 + 68, fault_addr);
    write_sigset(uc + 104, t->sigmask);
    W32(info, sig); W32(info + 8, fault_addr ? 1 : 0); W32(info + 12, fault_addr);
    uint64_t old_mask = t->sigmask;
    t->sigmask |= sa.mask | ((sa.flags & SA_NODEFER_) ? 0 : bit(sig));
    t->in_handler++;
    c->r[0] = sig; c->r[1] = info; c->r[2] = uc;
    c->r[13] = sp; c->r[14] = sigreturn_trap;
    call_addr(c, sa.handler);
    t->in_handler--;
    // sigreturn: restore the (possibly modified) context
    uint64_t newmask = read_sigset(uc + 104);
    uint32_t new_pc = R32(sc + 12 + 60);
    uint32_t r[15]; for (int k = 0; k < 15; k++) r[k] = R32(sc + 12 + 4 * k);
    uint32_t ncpsr = R32(sc + 12 + 64);
    *c = saved;
    for (int k = 0; k < 15; k++) c->r[k] = r[k];
    c->n = ncpsr >> 31; c->z = (ncpsr >> 30) & 1; c->cf = (ncpsr >> 29) & 1; c->v = (ncpsr >> 28) & 1;
    c->r[15] = new_pc;
    t->sigmask = newmask;
    (void)old_mask;
    c->sigpending = t->pending.load() & ~t->sigmask ? 1 : 0;
    return new_pc != ctx_pc;
}
}  // namespace

namespace rt {
bool deliver_pending(Thread *t) {
    bool any = false;
    for (;;) {
        uint64_t p = t->pending.load() & ~t->sigmask;
        if (!p) break;
        int sig = __builtin_ctzll(p) + 1;
        t->pending.fetch_and(~bit(sig));
        any = true;
        uint32_t pc = t->c->r[15];
        if (verbose > 1) logf("deliver signal %d on thread %u at %s", sig, t->tid, sym_name(pc));
        bool redirected = run_handler(t, sig, pc, 0);
        if (redirected && verbose) logf("signal %d handler redirected pc %s (ignored in recompiled code)", sig, sym_name(t->c->r[15]));
    }
    t->c->sigpending = (t->pending.load() & ~t->sigmask) ? 1 : 0;
    return any;
}
void wake(Thread *t) {
    std::condition_variable *w; std::mutex *wm;
    { std::lock_guard<std::mutex> g(t->mu); t->cv.notify_all(); w = t->waiting_on; wm = t->waiting_mu; }
    if (w) {                                   // sync objects are never freed, so the pointers stay valid
        if (wm) { std::lock_guard<std::mutex> g2(*wm); }
        w->notify_all();
    }
}
void signal_thread(Thread *t, int sig) {
    if (verbose > 1) logf("signal %d -> thread %u (masked %d)", sig, t->tid, (int)((t->sigmask & bit(sig)) != 0));
    t->pending.fetch_or(bit(sig));
    if (!(t->sigmask & bit(sig))) t->c->sigpending = 1;
    wake(t);
}
void deliver_fault(CPU *c, uint32_t pc, uint32_t addr) {
    Thread *t = (Thread *)c->thread;
    if (verbose > 1) logf("guest SIGSEGV at %s addr 0x%08x", sym_name(pc), addr);
    SigAct sa; { std::lock_guard<std::mutex> g(sig_mu); sa = sigacts[G_SIGSEGV]; }
    if (sa.handler <= 1) fatal("null access 0x%08x at %s (lr %s), no guest handler", addr, sym_name(pc), sym_name(c->r[14]));
    run_handler(t, G_SIGSEGV, pc, addr);
}
}  // namespace rt

extern "C" void guest_poll(CPU *c) {
    Thread *t = (Thread *)c->thread;
    if (!t) { c->sigpending = 0; return; }
    deliver_pending(t);
}

static void sig_init() {
    static bool done; if (done) return; done = true;
    sigreturn_trap = new_trap(sigreturn_fn, "__sigreturn");
}

IMP(sigaction) {
    sig_init();
    int sig = (int)A(0); uint32_t act = A(1), old = A(2);
    if (sig <= 0 || sig > 64) { gerr(G_EINVAL); RET(-1); return; }
    std::lock_guard<std::mutex> g(sig_mu);
    // bionic arm struct sigaction: handler, sa_mask (4 bytes), sa_flags, sa_restorer
    if (old) { W32(old, sigacts[sig].handler); W32(old + 4, (uint32_t)sigacts[sig].mask); W32(old + 8, sigacts[sig].flags); W32(old + 12, 0); }
    if (act) { sigacts[sig].handler = R32(act); sigacts[sig].mask = R32(act + 4); sigacts[sig].flags = R32(act + 8); }
    if (verbose && act) logf("sigaction(%d) handler %s flags %x", sig, sym_name(sigacts[sig].handler), sigacts[sig].flags);
    RET(0);
}
IMP(bsd_signal) {
    sig_init();
    int sig = (int)A(0);
    std::lock_guard<std::mutex> g(sig_mu);
    uint32_t old = sig > 0 && sig <= 64 ? sigacts[sig].handler : 0;
    if (sig > 0 && sig <= 64) { sigacts[sig].handler = A(1); sigacts[sig].flags = 0; sigacts[sig].mask = 0; }
    RET(old);
}
static void do_sigmask(CPU *c, int how, uint32_t set, uint32_t old) {
    Thread *t = self_thread();
    if (old) { W32(old, (uint32_t)t->sigmask); }
    if (set) {
        uint64_t s = R32(set);
        if (how == 0) t->sigmask |= s; else if (how == 1) t->sigmask &= ~s; else t->sigmask = s;
        t->sigmask &= ~(bit(9) | bit(19));
    }
    if (t->pending.load() & ~t->sigmask) { c->sigpending = 1; }
}
IMP(sigprocmask) { do_sigmask(c, (int)A(0), A(1), A(2)); RET(0); if (c->sigpending) guest_poll(c); }
IMP(pthread_sigmask) { do_sigmask(c, (int)A(0), A(1), A(2)); RET(0); if (c->sigpending) guest_poll(c); }
IMP(sigsuspend) {
    Thread *t = self_thread();
    uint64_t saved = t->sigmask;
    t->sigmask = R32(A(0));
    {
        std::unique_lock<std::mutex> lk(t->mu);
        while (!(t->pending.load() & ~t->sigmask)) t->cv.wait_for(lk, std::chrono::milliseconds(50));
    }
    deliver_pending(t);
    t->sigmask = saved;
    gerr(G_EINTR); RET(-1);
    if (t->pending.load() & ~t->sigmask) c->sigpending = 1;
}
IMP(pthread_kill) {
    uint32_t target = A(0); int sig = (int)A(1);
    Thread *t = thread_by_guest(target);
    if (!t) { RET(G_ESRCH); return; }
    if (sig) signal_thread(t, sig);
    RET(0);
    if (t == self_thread() && c->sigpending) guest_poll(c);
}
IMP(raise) { signal_thread(self_thread(), (int)A(0)); RET(0); guest_poll(c); }
IMP(kill) {
    int pid = (int)A(0), sig = (int)A(1);
    if (pid != 0 && pid != 1000 && pid != -1) { gerr(G_ESRCH); RET(-1); return; }
    if (sig) signal_thread(self_thread(), sig);
    RET(0);
    if (c->sigpending) guest_poll(c);
}
IMP(tkill) {
    uint32_t tid = A(0); int sig = (int)A(1);
    Thread *found = nullptr;
    { std::lock_guard<std::mutex> g(threads_mu); for (auto &kv : threads_by_self) if (kv.second->tid == tid) found = kv.second; }
    if (!found) { gerr(G_ESRCH); RET(-1); return; }
    if (sig) signal_thread(found, sig);
    RET(0);
}
IMP(sigaltstack) { if (A(1)) { W32(A(1), 0); W32(A(1) + 4, 2 /*SS_DISABLE*/); W32(A(1) + 8, 0); } RET(0); }

// ======================================================================= threads
namespace {
struct StartInfo { Thread *t; };
void thread_main(void *p) {
    Thread *t = ((StartInfo *)p)->t; delete (StartInfo *)p;
    attach(t);
    CPU *c = t->c;
    jmp_buf jb; t->exit_jb = &jb;
    uint32_t r;
    if (setjmp(jb) == 0) r = call_c(c, t->start_fn, {t->start_arg});
    else r = t->result;
    t->exit_jb = nullptr;
    std::lock_guard<std::mutex> g(t->mu);
    t->result = r; t->finished = true;
    t->done_cv.notify_all();
}
uint32_t attr_stacksize(uint32_t attr) { return attr ? R32(attr + 8) : 0; }
}

IMP(pthread_create) {
    uint32_t out = A(0), attr = A(1), fn = A(2), argp = A(3);
    uint32_t ss = attr_stacksize(attr);
    if (ss < (256u << 10)) ss = 1u << 20;
    Thread *t = make_thread(ss);
    t->start_fn = fn; t->start_arg = argp;
    t->detached = attr && (R32(attr) & 1);
    W32(out, t->self);
    if (verbose) logf("pthread_create %s (guest stack %u KB) -> thread %u", sym_name(fn), ss >> 10, t->tid);
    host::thread_start(thread_main, new StartInfo{t}, 64u << 20);
    RET(0);
}
IMP(pthread_self) { RET(self_thread()->self); }
IMP(pthread_equal) { RET(A(0) == A(1)); }
IMP(pthread_detach) { if (Thread *t = thread_by_guest(A(0))) t->detached = true; RET(0); }
IMP(pthread_join) {
    Thread *t = thread_by_guest(A(0));
    if (!t) { RET(G_ESRCH); return; }
    {
        std::unique_lock<std::mutex> lk(t->mu);
        while (!t->finished) {
            t->done_cv.wait_for(lk, std::chrono::milliseconds(20));
            if (c->sigpending) { lk.unlock(); guest_poll(c); lk.lock(); }
        }
    }
    if (A(1)) W32(A(1), t->result);
    RET(0);
}
IMP(pthread_exit) {
    Thread *t = self_thread();
    if (!t->exit_jb) fatal("pthread_exit on a thread not started by the guest (%s)", sym_name(c->r[14]));
    t->result = A(0);
    longjmp(*(jmp_buf *)t->exit_jb, 1);
}
IMP(gettid) { RET(self_thread()->tid); }
IMP(getpid) { RET(1000); }
IMP(pthread_attr_init) { memset(GPTR(A(0)), 0, 24); W32(A(0) + 8, 1u << 20); W32(A(0) + 12, 4096); RET(0); }
IMP(pthread_attr_destroy) { RET(0); }
IMP(pthread_attr_setstacksize) { W32(A(0) + 8, A(1)); RET(0); }
IMP(pthread_attr_getstacksize) { W32(A(1), R32(A(0) + 8)); RET(0); }
IMP(pthread_attr_setdetachstate) { W32(A(0), (R32(A(0)) & ~1u) | (A(1) ? 1u : 0u)); RET(0); }
IMP(pthread_attr_getdetachstate) { W32(A(1), R32(A(0)) & 1); RET(0); }
IMP(pthread_attr_setschedparam) { RET(0); }
IMP(pthread_attr_setschedpolicy) { RET(0); }
IMP(pthread_attr_getstack) { W32(A(1), R32(A(0) + 4)); W32(A(2), R32(A(0) + 8)); RET(0); }
IMP(pthread_getattr_np) {
    Thread *t = thread_by_guest(A(0)); if (!t) t = self_thread();
    uint32_t attr = A(1);
    memset(GPTR(attr), 0, 24);
    W32(attr + 4, t->stack_lo); W32(attr + 8, t->stack_hi - t->stack_lo); W32(attr + 12, 4096);
    RET(0);
}
IMP(pthread_getschedparam) { W32(A(1), 0); W32(A(2), 0); RET(0); }
IMP(pthread_setschedparam) { RET(0); }
IMP(sched_get_priority_max) { RET(0); }
IMP(sched_get_priority_min) { RET(0); }
IMP(sched_yield) { std::this_thread::yield(); RET(0); }
IMP(getpriority) { RET(0); }
IMP(setpriority) { RET(0); }

// ---- TLS keys
static std::mutex key_mu;
static uint32_t key_dtor[128];
static bool key_used[128];
IMP(pthread_key_create) {
    std::lock_guard<std::mutex> g(key_mu);
    for (uint32_t k = 1; k < 128; k++) if (!key_used[k]) { key_used[k] = true; key_dtor[k] = A(1); W32(A(0), k); RET(0); return; }
    RET(G_EAGAIN);
}
IMP(pthread_key_delete) { std::lock_guard<std::mutex> g(key_mu); if (A(0) < 128) key_used[A(0)] = false; RET(0); }
IMP(pthread_getspecific) { RET(A(0) < 128 ? self_thread()->keys[A(0)] : 0); }
IMP(pthread_setspecific) { if (A(0) < 128) self_thread()->keys[A(0)] = A(1); RET(0); }

// ---- once
static std::mutex once_mu;
IMP(pthread_once) {
    uint32_t ctl = A(0), fn = A(1);
    // 0 = not run, 1 = running, 2 = done
    std::unique_lock<std::mutex> lk(once_mu);
    for (;;) {
        uint32_t v = R32(ctl);
        if (v == 2) { RET(0); return; }
        if (v == 0) break;
        lk.unlock(); std::this_thread::yield(); lk.lock();
    }
    W32(ctl, 1); lk.unlock();
    call_c(c, fn, {});
    lk.lock(); W32(ctl, 2);
    RET(0);
}

// ======================================================================= mutex / cond / sem
namespace {
struct GMutex { std::mutex m; std::condition_variable cv; Thread *owner = nullptr; int count = 0; int type = 0; };
struct GCond { std::mutex m; std::condition_variable cv; uint64_t seq = 0; };
struct GSem { std::mutex m; std::condition_variable cv; uint32_t count = 0; };

const uint32_t TAG = 0x40000000u, TAG_MASK = 0xc0000000u;
std::mutex obj_mu;
std::vector<void *> objects(1, nullptr);
std::vector<uint32_t> free_slots;

uint32_t new_handle(void *p) {
    std::lock_guard<std::mutex> g(obj_mu);
    uint32_t k;
    if (!free_slots.empty()) { k = free_slots.back(); free_slots.pop_back(); objects[k] = p; }
    else { k = (uint32_t)objects.size(); objects.push_back(p); }
    return TAG | k;
}
template <class T> T *get_obj(uint32_t word_addr, T *(*make)(uint32_t init)) {
    uint32_t w = __atomic_load_n((uint32_t *)GPTR(word_addr), __ATOMIC_ACQUIRE);
    if ((w & TAG_MASK) == TAG) {
        std::lock_guard<std::mutex> g(obj_mu);
        uint32_t k = w & ~TAG_MASK;
        if (k < objects.size() && objects[k]) return (T *)objects[k];
    }
    T *o = make(w);
    uint32_t h = new_handle(o);
    uint32_t expect = w;
    if (!__atomic_compare_exchange_n((uint32_t *)GPTR(word_addr), &expect, h, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        // another thread initialized it first
        { std::lock_guard<std::mutex> g(obj_mu); objects[h & ~TAG_MASK] = nullptr; free_slots.push_back(h & ~TAG_MASK); }
        delete o;
        return get_obj<T>(word_addr, make);
    }
    return o;
}
void drop_obj(uint32_t word_addr) {
    uint32_t w = R32(word_addr);
    if ((w & TAG_MASK) == TAG) {
        std::lock_guard<std::mutex> g(obj_mu);
        uint32_t k = w & ~TAG_MASK;
        if (k < objects.size() && objects[k]) { /* leaked on purpose: other threads may still race on it */ objects[k] = nullptr; free_slots.push_back(k); }
    }
    W32(word_addr, 0);
}
GMutex *mk_mutex(uint32_t init) {
    GMutex *m = new GMutex();
    // bionic initializers: 0 normal, 0x4000 recursive, 0x8000 errorcheck
    m->type = (init & 0xc000) == 0x4000 ? 1 : (init & 0xc000) == 0x8000 ? 2 : 0;
    return m;
}
GCond *mk_cond(uint32_t) { return new GCond(); }
GSem *mk_sem(uint32_t) { return new GSem(); }
GMutex *mutex_of(uint32_t a) { return get_obj<GMutex>(a, mk_mutex); }

int mutex_lock(GMutex *m, Thread *t, bool try_only) {
    std::unique_lock<std::mutex> lk(m->m);
    if (m->owner == t) {
        if (m->type == 1) { m->count++; return 0; }
        if (m->type == 2) return G_EDEADLK;
        if (try_only) return G_EBUSY;
        fatal("guest deadlock: thread relocks a normal mutex");
    }
    if (try_only && m->owner) return G_EBUSY;
    while (m->owner) m->cv.wait(lk);
    m->owner = t; m->count = 1;
    return 0;
}
int mutex_unlock(GMutex *m, Thread *t) {
    std::lock_guard<std::mutex> lk(m->m);
    if (m->owner != t) return G_EPERM;
    if (--m->count == 0) { m->owner = nullptr; m->cv.notify_one(); }
    return 0;
}
}  // namespace

IMP(pthread_mutexattr_init) { W32(A(0), 0); RET(0); }
IMP(pthread_mutexattr_destroy) { RET(0); }
IMP(pthread_mutexattr_settype) { W32(A(0), A(1)); RET(0); }
IMP(pthread_mutex_init) {
    uint32_t type = A(1) ? R32(A(1)) : 0;
    W32(A(0), type == 1 ? 0x4000 : type == 2 ? 0x8000 : 0);
    RET(0);
}
IMP(pthread_mutex_destroy) { drop_obj(A(0)); RET(0); }
IMP(pthread_mutex_lock) { RET(mutex_lock(mutex_of(A(0)), self_thread(), false)); }
IMP(pthread_mutex_trylock) { RET(mutex_lock(mutex_of(A(0)), self_thread(), true)); }
IMP(pthread_mutex_unlock) { RET(mutex_unlock(mutex_of(A(0)), self_thread())); }

static int cond_wait(CPU *c, uint32_t condp, uint32_t mutp, int64_t deadline_ns) {
    Thread *t = self_thread();
    GCond *cv = get_obj<GCond>(condp, mk_cond);
    GMutex *m = mutex_of(mutp);
    int rc = 0;
    std::unique_lock<std::mutex> lk(cv->m);
    uint64_t seq0 = cv->seq;
    // release the guest mutex completely
    int saved_count;
    {
        std::lock_guard<std::mutex> g(m->m);
        if (m->owner != t) return G_EPERM;
        saved_count = m->count; m->owner = nullptr; m->count = 0; m->cv.notify_one();
    }
    { std::lock_guard<std::mutex> g(t->mu); t->waiting_on = &cv->cv; t->waiting_mu = &cv->m; }
    bool sig = false;
    for (;;) {
        if (cv->seq != seq0) break;
        if (c->sigpending) { sig = true; break; }
        auto step = std::chrono::milliseconds(100);
        if (deadline_ns) {
            int64_t now = host::now_us() * 1000;
            if (now >= deadline_ns) { rc = G_ETIMEDOUT; break; }
            int64_t left = deadline_ns - now;
            if (left < 100000000) step = std::chrono::milliseconds(left / 1000000 + 1);
        }
        cv->cv.wait_for(lk, step);
    }
    lk.unlock();
    { std::lock_guard<std::mutex> g(t->mu); t->waiting_on = nullptr; t->waiting_mu = nullptr; }
    if (sig) guest_poll(c);       // handler runs without the guest mutex held (like a real signal)
    mutex_lock(m, t, false);
    { std::lock_guard<std::mutex> g(m->m); m->count = saved_count; }
    return rc;
}
IMP(pthread_cond_init) { W32(A(0), 0); RET(0); }
IMP(pthread_cond_destroy) { drop_obj(A(0)); RET(0); }
IMP(pthread_cond_wait) { RET(cond_wait(c, A(0), A(1), 0)); }
IMP(pthread_cond_timedwait) {
    uint32_t ts = A(2);
    int64_t dl = (int64_t)R32(ts) * 1000000000ll + R32(ts + 4);   // CLOCK_REALTIME
    RET(cond_wait(c, A(0), A(1), dl ? dl : 1));
}
IMP(pthread_cond_timedwait_relative_np) {
    uint32_t ts = A(2);
    int64_t dl = host::now_us() * 1000 + (int64_t)R32(ts) * 1000000000ll + R32(ts + 4);
    RET(cond_wait(c, A(0), A(1), dl));
}
IMP(pthread_cond_signal) {
    GCond *cv = get_obj<GCond>(A(0), mk_cond);
    { std::lock_guard<std::mutex> g(cv->m); cv->seq++; }
    cv->cv.notify_one(); RET(0);
}
IMP(pthread_cond_broadcast) {
    GCond *cv = get_obj<GCond>(A(0), mk_cond);
    { std::lock_guard<std::mutex> g(cv->m); cv->seq++; }
    cv->cv.notify_all(); RET(0);
}

// semaphores (sem_t is one word in bionic)
static int sem_wait_impl(CPU *c, uint32_t sp, int64_t deadline_ns, bool try_only) {
    Thread *t = self_thread();
    GSem *s = get_obj<GSem>(sp, mk_sem);
    std::unique_lock<std::mutex> lk(s->m);
    { std::lock_guard<std::mutex> g(t->mu); t->waiting_on = &s->cv; t->waiting_mu = &s->m; }
    int rc = 0;
    while (s->count == 0) {
        if (try_only) { rc = G_EAGAIN; break; }
        if (c->sigpending) { rc = G_EINTR; break; }
        auto step = std::chrono::milliseconds(100);
        if (deadline_ns) {
            int64_t now = host::now_us() * 1000;
            if (now >= deadline_ns) { rc = G_ETIMEDOUT; break; }
        }
        s->cv.wait_for(lk, step);
    }
    if (!rc) s->count--;
    lk.unlock();
    { std::lock_guard<std::mutex> g(t->mu); t->waiting_on = nullptr; t->waiting_mu = nullptr; }
    if (rc == G_EINTR) guest_poll(c);
    return rc;
}
IMP(sem_init) { W32(A(0), 0); GSem *s = get_obj<GSem>(A(0), mk_sem); s->count = A(2); RET(0); }
IMP(sem_destroy) { drop_obj(A(0)); RET(0); }
IMP(sem_post) {
    GSem *s = get_obj<GSem>(A(0), mk_sem);
    { std::lock_guard<std::mutex> g(s->m); s->count++; }
    s->cv.notify_one(); RET(0);
}
IMP(sem_wait) { int rc = sem_wait_impl(c, A(0), 0, false); if (rc) { gerr(rc); RET(-1); } else RET(0); }
IMP(sem_trywait) { int rc = sem_wait_impl(c, A(0), 0, true); if (rc) { gerr(rc); RET(-1); } else RET(0); }
IMP(sem_timedwait) {
    uint32_t ts = A(1);
    int64_t dl = (int64_t)R32(ts) * 1000000000ll + R32(ts + 4);
    int rc = sem_wait_impl(c, A(0), dl ? dl : 1, false); if (rc) { gerr(rc); RET(-1); } else RET(0);
}
IMP(sem_getvalue) { GSem *s = get_obj<GSem>(A(0), mk_sem); W32(A(1), s->count); RET(0); }

// ======================================================================= sleeping (interruptible)
static void sleep_ns(CPU *c, int64_t ns) {
    Thread *t = self_thread();
    int64_t end = host::mono_ns() + ns;
    for (;;) {
        int64_t left = end - host::mono_ns();
        if (left <= 0) break;
        {
            std::unique_lock<std::mutex> lk(t->mu);
            if (!c->sigpending) t->cv.wait_for(lk, std::chrono::nanoseconds(left));
        }
        if (c->sigpending) guest_poll(c);
    }
}
IMP(nanosleep) { uint32_t ts = A(0); sleep_ns(c, (int64_t)R32(ts) * 1000000000ll + R32(ts + 4)); if (A(1)) { W32(A(1), 0); W32(A(1) + 4, 0); } RET(0); }
IMP(usleep) { sleep_ns(c, (int64_t)A(0) * 1000); RET(0); }
IMP(sleep) { sleep_ns(c, (int64_t)A(0) * 1000000000ll); RET(0); }

// ======================================================================= raw syscalls
extern "C" void guest_svc(CPU *c) {
    uint32_t nr = c->r[7];
    switch (nr) {
    case 0xf0002: c->r[0] = 0; return;                 // cacheflush
    case 224: c->r[0] = self_thread()->tid; return;    // gettid
    case 20: c->r[0] = 1000; return;                   // getpid
    case 158: std::this_thread::yield(); c->r[0] = 0; return;   // sched_yield
    case 238: {                                        // tkill
        Thread *found = nullptr; uint32_t tid = c->r[0];
        { std::lock_guard<std::mutex> g(threads_mu); for (auto &kv : threads_by_self) if (kv.second->tid == tid) found = kv.second; }
        if (found && c->r[1]) signal_thread(found, (int)c->r[1]);
        c->r[0] = found ? 0 : (uint32_t)-3; return;
    }
    }
    fatal("guest syscall %u at %s", nr, sym_name(c->r[15]));
}
IMP(syscall) {
    // syscall(nr, a0, a1, ...) -> svc ABI
    CPU tmp = *c;
    tmp.r[7] = A(0); tmp.r[0] = A(1); tmp.r[1] = A(2); tmp.r[2] = A(3); tmp.r[3] = A(4);
    tmp.thread = c->thread;
    guest_svc(&tmp);
    c->r[0] = tmp.r[0];
    if ((int32_t)c->r[0] < 0 && (int32_t)c->r[0] > -4096) { gerr(-(int32_t)c->r[0]); c->r[0] = (uint32_t)-1; }
}

uint32_t main_stack_top() { return rt::g_main_stack_top; }
