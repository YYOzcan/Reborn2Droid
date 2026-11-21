// Guest threads and signals.
#pragma once
#include "rt.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace rt {
struct Thread {
    CPU *c = nullptr;
    uint32_t self = 0;              // guest pthread_t (address of a small guest block)
    uint32_t tid = 0;               // fake Linux tid
    uint32_t stack_lo = 0, stack_hi = 0;
    uint32_t start_fn = 0, start_arg = 0, result = 0;
    bool detached = false, finished = false, is_main = false;
    std::mutex mu;                  // protects the fields below + wakeups
    std::condition_variable cv;     // thread is waiting (sleep/sigsuspend/join): wake on signal
    std::condition_variable *waiting_on = nullptr;   // a condition variable it is blocked on
    std::mutex *waiting_mu = nullptr;
    std::atomic<uint64_t> pending{0};   // pending guest signals (bit n = signal n)
    uint64_t sigmask = 0;           // blocked guest signals
    uint32_t keys[128] = {0};       // pthread_getspecific values
    uint32_t errno_addr = 0;
    std::condition_variable done_cv;
    int in_handler = 0;
    void *exit_jb = nullptr;        // host jmp_buf of the thread's start frame (pthread_exit)
};

Thread *self_thread();                        // creates the record for foreign host threads
Thread *thread_by_guest(uint32_t pthread_self);
Thread *main_thread_create(size_t guest_stack);   // call once on the host main/game thread
bool deliver_pending(Thread *t);              // run handlers for pending unblocked signals (true if any ran)
void wake(Thread *t);                         // interrupt a blocking wait of t
}
