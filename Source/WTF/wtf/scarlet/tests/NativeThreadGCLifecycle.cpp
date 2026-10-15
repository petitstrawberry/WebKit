/*
 * Native Scarlet WTF thread/GC lifetime regression. Link against the same WTF,
 * JavaScriptCore and native runtime closure as ScarletWebViewSmoke.
 */
#include "config.h"
#include "JavaScript.h"
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Threading.h>
#include <wtf/RefPtr.h>
#include <wtf/scarlet/NativeThreadGC.h>
#include <atomic>
#include <cerrno>
#include <cstdio>

// Existing JSC test API: unlike JSGarbageCollect's scheduling hint, this
// performs an actual synchronous full collection.
extern "C" void JSSynchronousGarbageCollectForDebugging(JSContextRef);

namespace {
constexpr uint64_t stackCookie = 0x43534743524f4f54ULL;
struct State {
    std::atomic<JSGlobalContextRef> context { nullptr };
    std::atomic<bool> ready { false }, stopCollector { false }, releaseRoot { false }, rootPassed { false }, failed { false };
    std::atomic<unsigned> finalizations { 0 }, requested { 0 }, completed { 0 };
};
void yield()
{
    scarlet_native_call(21, 0, 0, 0, 0, 0);
}
template<typename Predicate> bool waitFor(Predicate&& predicate)
{
    auto deadline = WTF::MonotonicTime::now() + WTF::Seconds { 30 };
    while (!predicate()) {
        if (WTF::MonotonicTime::now() >= deadline)
            return false;
        yield();
    }
    return true;
}
void finalizeRoot(JSObjectRef object)
{
    static_cast<std::atomic<unsigned>*>(JSObjectGetPrivate(object))->fetch_add(1);
}
void holdRoot(State& state)
{
    auto context = JSGlobalContextCreate(nullptr);
    JSClassDefinition definition = kJSClassDefinitionEmpty;
    definition.finalize = finalizeRoot;
    auto rootClass = JSClassCreate(&definition);
    JSObjectRef volatile root = JSObjectMake(context, rootClass, &state.finalizations);
    auto marker = JSStringCreateWithUTF8CString("marker");
    JSValueRef exception { nullptr };
    JSObjectSetProperty(context, root, marker, JSValueMakeNumber(context, 42), kJSPropertyAttributeNone, &exception);
    volatile uint64_t cookie = stackCookie;
    state.context.store(context, std::memory_order_release);
    state.failed.store(exception, std::memory_order_release);
    state.ready.store(true, std::memory_order_release);
    // The object is deliberately not JSValueProtect'ed, stored in a JS global,
    // or sent to the collector: only this live native stack/register roots it.
    while (!state.releaseRoot.load(std::memory_order_acquire))
        yield();
    bool passed = cookie == stackCookie && !state.finalizations.load() && !exception;
    if (passed) {
        auto value = JSObjectGetProperty(context, root, marker, &exception);
        passed = !exception && JSValueToNumber(context, value, &exception) == 42 && !exception;
    }
    state.rootPassed.store(passed, std::memory_order_release);
    JSStringRelease(marker);
    root = nullptr;
    JSGlobalContextRelease(context);
    JSClassRelease(rootClass);
}
bool sample(WTF::Thread& thread)
{
    WTF::ThreadSuspendLocker locker;
    auto suspended = thread.suspend(locker);
    if (!suspended)
        return false;
    WTF::PlatformRegisters registers;
    auto size = thread.getRegisters(locker, registers);
    bool passed = size == sizeof(registers) && registers.pc && thread.stack().contains(reinterpret_cast<void*>(registers.sp));
    thread.resume(locker);
    return passed;
}
bool exited(WTF::Thread& thread)
{
    WTF::ThreadSuspendLocker locker;
    auto result = thread.suspend(locker);
    if (result) {
        thread.resume(locker);
        return false;
    }
    return result.error() == ESRCH;
}
} // namespace

int main()
{
    WTF::initializeMainThread();
    State state;
    auto rootThread = WTF::Thread::create("GC live stack root"_s, [&] { holdRoot(state); });
    if (!waitFor([&] { return state.ready.load(std::memory_order_acquire); }))
        return 1; // The outer native verifier must also enforce its process timeout.
    auto context = state.context.load(std::memory_order_acquire);
    auto collector = WTF::Thread::create("GC lifecycle collector"_s, [&] {
        unsigned completed = 0;
        while (!state.stopCollector.load(std::memory_order_acquire)) {
            if (completed == state.requested.load(std::memory_order_acquire)) {
                yield();
                continue;
            }
            JSSynchronousGarbageCollectForDebugging(context);
            if (state.finalizations.load() || !sample(rootThread.get()))
                state.failed.store(true, std::memory_order_release);
            state.completed.store(++completed, std::memory_order_release);
        }
    });
    WTF::RefPtr<WTF::Thread> firstExited;
    unsigned threads = 0;
    for (; threads < 1024 && !state.failed.load(std::memory_order_acquire); ++threads) {
        auto thread = WTF::Thread::create("GC registry churn"_s, [] { });
        // Race the actual TLS unregister path against a real GC scanner.
        if (!((threads + 1) % 16))
            state.requested.fetch_add(1, std::memory_order_release);
        if (thread->waitForCompletion() || !exited(thread.get())) {
            state.failed.store(true, std::memory_order_release);
            break;
        }
        if (!firstExited)
            firstExited = thread.ptr();
        if (!((threads + 1) % 16)
            && !waitFor([&] { return state.completed.load(std::memory_order_acquire) == state.requested.load(std::memory_order_acquire); })) {
            state.failed.store(true, std::memory_order_release);
            break;
        }
    }
    // An old Thread is retained across 1024 slot reuses. A late suspend must
    // reject it even on a POSIX runtime which reuses pthread_t identities.
    bool oldThreadRejected = firstExited && exited(*firstExited);
    state.stopCollector.store(true, std::memory_order_release);
    int collectorJoin = collector->waitForCompletion();
    state.releaseRoot.store(true, std::memory_order_release);
    int rootJoin = rootThread->waitForCompletion();
    bool passed = threads == 1024 && state.completed.load() == 64 && !state.failed.load()
        && oldThreadRejected && !collectorJoin && !rootJoin && state.rootPassed.load()
        && state.finalizations.load() == 1;
    std::printf("SCARLET_NATIVE_GC_LIFECYCLE threads=%u collections=%u finalized=%u old_thread_esrch=%u pass=%u\n",
        threads, state.completed.load(), state.finalizations.load(), static_cast<unsigned>(oldThreadRejected), static_cast<unsigned>(passed));
    scarlet_gc_print_statistics();
    return passed ? 0 : 1;
}
