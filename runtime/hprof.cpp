// Host sampling profiler (Linux dev builds): RB_HPROF=1 samples the host pc of whichever thread burns CPU.
#if !defined(__ANDROID__) && defined(__linux__)
#include "rt.h"
#include <csignal>
#include <sys/time.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <map>
#include <atomic>
#include <algorithm>
#include <cxxabi.h>

namespace {
const int N = 1 << 20;
uintptr_t *samples; std::atomic<int> count{0};
void on_prof(int, siginfo_t *, void *uc_) {
    ucontext_t *uc = (ucontext_t *)uc_;
    int k = count.fetch_add(1);
    if (k < N) samples[k] = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
}
struct Init {
    Init() {
        if (!getenv("RB_HPROF")) return;
        samples = new uintptr_t[N];
        struct sigaction sa{}; sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, nullptr);
        itimerval tv{{0, 500}, {0, 500}};
        setitimer(ITIMER_PROF, &tv, nullptr);
    }
} init;
}
void hprof_dump() {
    if (!samples) return;
    itimerval tv{}; setitimer(ITIMER_PROF, &tv, nullptr);
    int n = std::min(count.load(), N);
    std::map<std::string, int> by;
    for (int i = 0; i < n; i++) {
        Dl_info di; std::string name = "?";
        if (dladdr((void *)samples[i], &di) && di.dli_sname) {
            int st; char *d = abi::__cxa_demangle(di.dli_sname, nullptr, nullptr, &st);
            name = d ? d : di.dli_sname; free(d);
            if (name.size() > 90) name = name.substr(0, 90);
        } else if (dladdr((void *)samples[i], &di) && di.dli_fname) name = std::string("[") + di.dli_fname + "]";
        by[name]++;
    }
    std::vector<std::pair<int, std::string>> v; for (auto &kv : by) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    rt::logf("host profile: %d samples", n);
    for (size_t k = 0; k < v.size() && k < 45; k++) rt::logf("  %5.1f%% %s", 100.0 * v[k].first / n, v[k].second.c_str());
}
#else
void hprof_dump() {}
#endif
