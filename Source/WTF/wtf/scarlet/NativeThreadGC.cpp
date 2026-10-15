#include "config.h"
#include <wtf/scarlet/NativeThreadGC.h>

#include <atomic>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

// Bound concurrent registrations, not the lifetime number of created threads.
// A slot is retired by its own exiting thread only after it has left the WTF
// ThreadGroups and while holding ThreadSuspendLocker. A scanner's record and
// saved-register pointers therefore remain stable through suspend and resume.
struct Record {
    std::atomic<unsigned long> id { 0 };
    unsigned long tid { };
    std::atomic<unsigned> state { 0 }; // 0 idle, 1 requested, 2 stopped, 3 resume, 4 acknowledged.
    ScarletGCRegisters registers { };
};

static Record records[256];
static std::atomic<unsigned long> suspensions { 0 };
static std::atomic<unsigned long> resumes { 0 };
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<Record*> request { nullptr };

static unsigned long call(unsigned long n, unsigned long a = 0, unsigned long b = 0, unsigned long c = 0, unsigned long d = 0, unsigned long e = 0)
{
    return scarlet_native_call(n, a, b, c, d, e);
}

extern "C" void scarlet_gc_event_entry();
extern "C" void scarlet_gc_event_return();

static Record* lookup(pthread_t id)
{
    for (auto& r : records) {
        if (r.id.load(std::memory_order_acquire) == id)
            return &r;
    }
    return nullptr;
}

extern "C" int scarlet_gc_register_current()
{
    auto id = pthread_self();
    if (lookup(id))
        return 0;

    pthread_mutex_lock(&registry_mutex);
    Record* free = nullptr;
    for (auto& r : records) {
        if (!r.id.load()) {
            free = &r;
            break;
        }
    }
    if (!free) {
        pthread_mutex_unlock(&registry_mutex);
        return EAGAIN;
    }

    // Register the native process-control handler with its executable restorer.
    if (call(644, 0, (unsigned long)scarlet_gc_event_entry, 0, 0, (unsigned long)scarlet_gc_event_return) == ~0ul) {
        pthread_mutex_unlock(&registry_mutex);
        return EINVAL;
    }
    free->tid = call(7);
    free->id.store(id, std::memory_order_release);
    pthread_mutex_unlock(&registry_mutex);
    return 0;
}

extern "C" int scarlet_gc_unregister_current()
{
    // The caller holds WTF's global ThreadSuspendLocker. Never take that lock
    // while still holding a ThreadGroup lock: didExit() must have finished first.
    pthread_mutex_lock(&registry_mutex);
    auto r = lookup(pthread_self());
    if (!r) {
        pthread_mutex_unlock(&registry_mutex);
        return ESRCH;
    }
    if (r->state.load(std::memory_order_acquire)) {
        pthread_mutex_unlock(&registry_mutex);
        return EBUSY;
    }
    // resume() acknowledged the delivered event before returning to state 0;
    // this target cannot reach its TLS destructor until the handler has returned.
    Record* expected = r;
    request.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    r->tid = 0;
    r->id.store(0, std::memory_order_release);
    pthread_mutex_unlock(&registry_mutex);
    return 0;
}

extern "C" void scarlet_gc_event_handler(const void*, unsigned long subtype, const uint64_t* context, const uint64_t* fp)
{
    // No malloc, locks, Rust TLS, stdio, or pthread calls in this async path.
    Record* r = request.load(std::memory_order_acquire);
    if (subtype != 271 || !r || r->tid != call(7) || r->state.load() != 1)
        _Exit(190);

    for (unsigned i = 0; i < 31; ++i)
        r->registers.gpr[i] = context[i];
    r->registers.pc = context[31];
    r->registers.sp = context[32];
    r->registers.nzcv = context[33];
    for (unsigned i = 0; i < 64; ++i)
        r->registers.simd[i] = fp[i];
    r->registers.fpcr = fp[64];
    r->registers.fpsr = fp[65];

    suspensions.fetch_add(1, std::memory_order_relaxed);
    r->state.store(2, std::memory_order_release);
    while (r->state.load(std::memory_order_acquire) != 3)
        call(21);
    r->state.store(4, std::memory_order_release);
}

extern "C" int scarlet_gc_suspend(pthread_t id, ScarletGCRegisters** result)
{
    auto r = lookup(id);
    if (!r || id == pthread_self())
        return ESRCH;

    unsigned expected = 0;
    if (!r->state.compare_exchange_strong(expected, 1))
        return EBUSY;
    request.store(r, std::memory_order_release);
    // Reliable, high-priority native process-control User(15) event.
    if (call(615, r->tid, 271, 1, 3) == ~0ul) {
        r->state.store(0);
        return ESRCH;
    }
    for (unsigned i = 0; i < 1000000; ++i) {
        if (r->state.load(std::memory_order_acquire) == 2) {
            *result = &r->registers;
            return 0;
        }
        call(21);
    }

    // A delivered event must never be abandoned with the target potentially stopped.
    _Exit(191);
}

extern "C" int scarlet_gc_resume(pthread_t id)
{
    auto r = lookup(id);
    if (!r || r->state.load() != 2)
        return EINVAL;

    r->state.store(3, std::memory_order_release);
    for (unsigned i = 0; i < 1000000; ++i) {
        if (r->state.load(std::memory_order_acquire) == 4) {
            r->state.store(0, std::memory_order_release);
            resumes.fetch_add(1);
            return 0;
        }
        call(21);
    }
    _Exit(192);
}

extern "C" int scarlet_gc_stack_bounds(void** low, void** high)
{
    unsigned long sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    unsigned long bounds[2];
    if (call(703, sp, (unsigned long)bounds) == ~0ul)
        return EINVAL;
    *low = (void*)bounds[0];
    *high = (void*)bounds[1];
    return 0;
}

extern "C" void scarlet_gc_print_statistics()
{
    printf("NATIVE_GC_STATS suspended=%lu resumed=%lu\n", suspensions.load(), resumes.load());
}
