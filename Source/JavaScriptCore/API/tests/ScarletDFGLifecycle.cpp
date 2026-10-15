// Standalone native Scarlet JSC regression. Link against the normal JSC/WTF
// closure; run once with JSC_useDFGJIT=false and once with it true.
// The restricted $vm helpers are enabled only in this test process.
#include "config.h"
#include "APICast.h"
#include "InitializeThreading.h"
#include "JSCConfig.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "JavaScript.h"
#include "Options.h"
#include "VM.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <pthread.h>
#include <sched.h>

using Clock = std::chrono::steady_clock;
static std::atomic<bool> observedTier { false };
static bool expectDFG;

static bool evaluate(JSGlobalContextRef context, const char* source, bool& result)
{
    auto script = JSStringCreateWithUTF8CString(source);
    JSValueRef exception = nullptr;
    auto value = JSEvaluateScript(context, script, nullptr, nullptr, 1, &exception);
    JSStringRelease(script);
    if (exception || !value) {
        auto text = exception ? JSValueToStringCopy(context, exception, nullptr) : nullptr;
        char message[1024] { };
        if (text) {
            JSStringGetUTF8CString(text, message, sizeof(message));
            JSStringRelease(text);
        }
        std::fprintf(stderr, "SCARLET_DFG_FAIL stage=evaluate exception=%s\n", message);
        return false;
    }
    result = JSValueToBoolean(context, value);
    return true;
}

static bool require(JSGlobalContextRef context, const char* source, const char* stage)
{
    bool result = false;
    if (!evaluate(context, source, result) || !result) {
        std::fprintf(stderr, "SCARLET_DFG_FAIL stage=%s\n", stage);
        return false;
    }
    return true;
}

static bool semantics(JSGlobalContextRef context)
{
    if (!require(context, R"JS(
        globalThis.expectedDFG = EXPECT_DFG;
        globalThis.tierSeen = false;
        globalThis.proto = { value: 7 };
        globalThis.receiver = Object.create(proto);
        function hot(value) {
            if (expectedDFG ? $vm.dfgTrue() : $vm.baselineJITTrue())
                tierSeen = true;
            return receiver.value + value;
        }
        $vm.noInline(hot);
        true;
    )JS", "setup"))
        return false;
    auto deadline = Clock::now() + std::chrono::seconds(20);
    bool seen = false;
    do {
        if (!require(context,
                "for (let n=0; n<1024; ++n) { if(hot(n)!==n+7) throw Error('warm result'); } true;",
                "warm-semantics")
            || !evaluate(context, "tierSeen", seen))
            return false;
        sched_yield();
    } while (!seen && Clock::now() < deadline);
    if (!seen) {
        std::fprintf(stderr, "SCARLET_DFG_FAIL stage=actual-tier expected=%s\n", expectDFG ? "DFG" : "Baseline");
        return false;
    }
    if (!require(context, R"JS(
        if (hot(2147483647)!==2147483654) throw Error('int overflow exit');
        if (hot(0.5)!==7.5 || hot('x')!=='7x') throw Error('type exit');
        proto.value = 11;
        if(hot(3)!==14) throw Error('prototype watchpoint');
        Object.defineProperty(proto,'value',{get(){return 19},configurable:true});
        if(hot(2)!==21) throw Error('getter invalidation');
        function numeric(x) { return [x+1, x*0, x/0]; }
        $vm.noInline(numeric);
        for(let n=0;n<5000;++n) numeric(n);
        if(numeric(2147483647)[0]!==2147483648) throw Error('int overflow');
        if(!Object.is(numeric(-1)[1],-0)) throw Error('negative zero');
        if(!Number.isNaN(numeric(NaN)[0])) throw Error('NaN');
        let caught = false;
        try { hot(Symbol()); } catch(e) { caught = e instanceof TypeError; }
        if(!caught) throw Error('optimized exception');
        globalThis.gcTierSeen = false;
        globalThis.gcAfterTierSeen = false;
        function acrossGC(n, collect) {
            const object = { value:n, data:new Uint8Array([3,5,7]) };
            const before = expectedDFG ? $vm.dfgTrue() : $vm.baselineJITTrue();
            if(before) gcTierSeen=true;
            if(collect) $vm.gc();
            const after = expectedDFG ? $vm.dfgTrue() : $vm.baselineJITTrue();
            if(collect && before && after) gcAfterTierSeen=true;
            return object.value===n && object.data[1]===5;
        }
        $vm.noInline(acrossGC);
        true;
    )JS", "osr-watchpoint-exception"))
        return false;
    deadline = Clock::now() + std::chrono::seconds(20);
    seen = false;
    do {
        if (!require(context,
                "for(let n=0;n<1024;++n) if(!acrossGC(n,(n&63)===0)) throw Error('object lifetime'); true;",
                "gc-warm")
            || !evaluate(context, "gcTierSeen", seen))
            return false;
        sched_yield();
    } while (!seen && Clock::now() < deadline);
    if (!seen || !require(context,
            "for(let n=0;n<32;++n) if(!acrossGC(n,true)) throw Error('GC root'); gcAfterTierSeen;",
            "full-gc-roots"))
        return false;
    std::printf("SCARLET_DFG_PASS stage=semantics tier=%s type-exit prototype-watchpoint exception full-gc32\n", expectDFG ? "DFG" : "Baseline");
    return true;
}

static JSValueRef tierReached(JSContextRef context, JSObjectRef, JSObjectRef, size_t count, const JSValueRef values[], JSValueRef*)
{
    if (count == 2 && JSValueToBoolean(context, values[expectDFG ? 0 : 1]))
        observedTier.store(true, std::memory_order_release);
    return JSValueMakeUndefined(context);
}

struct Interrupt {
    JSC::VM* vm;
    std::atomic<bool> done { false };
    std::atomic<bool> timedOut { false };
    std::atomic<bool> requested { false };
};

static void* interruptAfterTier(void* argument)
{
    auto& interrupt = *static_cast<Interrupt*>(argument);
    auto deadline = Clock::now() + std::chrono::seconds(25);
    while (!interrupt.done.load(std::memory_order_acquire)) {
        if (observedTier.load(std::memory_order_acquire)) {
            interrupt.requested.store(true, std::memory_order_release);
            interrupt.vm->notifyNeedTermination();
            return nullptr;
        }
        if (Clock::now() >= deadline) {
            interrupt.timedOut.store(true, std::memory_order_release);
            interrupt.vm->notifyNeedTermination();
            return nullptr;
        }
        sched_yield();
    }
    return nullptr;
}

static bool interruptLoop(JSGlobalContextRef context)
{
    auto name = JSStringCreateWithUTF8CString("nativeTierReached");
    auto callback = JSObjectMakeFunctionWithCallback(context, name, tierReached);
    JSObjectSetProperty(context, JSContextGetGlobalObject(context), name, callback, kJSPropertyAttributeReadOnly, nullptr);
    JSStringRelease(name);
    auto& vm = toJS(context)->vm();
    {
        JSC::JSLockHolder locker(vm);
        vm.ensureTerminationException();
    }
    observedTier.store(false, std::memory_order_release);
    Interrupt interrupt { &vm };
    pthread_t thread;
    if (pthread_create(&thread, nullptr, interruptAfterTier, &interrupt)) {
        std::fprintf(stderr, "SCARLET_DFG_FAIL stage=interrupt-thread\n");
        return false;
    }
    auto script = JSStringCreateWithUTF8CString(R"JS(
        function infiniteDFG() {
            for(;;) nativeTierReached($vm.dfgTrue(), $vm.baselineJITTrue());
        }
        $vm.noInline(infiniteDFG);
        infiniteDFG();
    )JS");
    JSValueRef exception = nullptr;
    auto result = JSEvaluateScript(context, script, nullptr, nullptr, 1, &exception);
    JSStringRelease(script);
    interrupt.done.store(true, std::memory_order_release);
    pthread_join(thread, nullptr);
    // A caught JS error or an arbitrary execution deadline cannot pass this:
    // termination is sent only after the infinite function executes that tier.
    bool passed = observedTier.load(std::memory_order_acquire)
        && interrupt.requested.load(std::memory_order_acquire)
        && !interrupt.timedOut.load(std::memory_order_acquire) && !result
        && vm.hasTerminationRequest();
    std::printf("SCARLET_DFG_%s stage=infinite-interrupt tier=%s observed=%u timeout=%u exception=%u\n",
        passed ? "PASS" : "FAIL", expectDFG ? "DFG" : "Baseline",
        observedTier.load(), interrupt.timedOut.load(), !!exception);
    return passed;
}

int main()
{
    static_assert(ENABLE_DFG_JIT, "Build this regression against the normal DFG-enabled native JSC");
    static_assert(!ENABLE_FTL_JIT && !ENABLE_WEBASSEMBLY && !ENABLE_YARR_JIT);
    JSC::Config::enableRestrictedOptions();
    JSC::Options::initializeWithOptionsCustomization([] {
        JSC::Options::useDollarVM() = true;
    });
    JSC::initialize();
    expectDFG = JSC::Options::useDFGJIT();
    if (!JSC::Options::useJIT() || !JSC::Options::useBaselineJIT() || !JSC::Options::useConcurrentJIT()
        || !JSC::Options::usePollingTraps() || JSC::Options::useFTLJIT()) {
        std::fprintf(stderr, "SCARLET_DFG_FAIL stage=options\n");
        return 1;
    }
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        auto context = JSGlobalContextCreate(nullptr);
        if (!context)
            return 1;
        auto expected = JSStringCreateWithUTF8CString("EXPECT_DFG");
        JSObjectSetProperty(context, JSContextGetGlobalObject(context), expected,
            JSValueMakeBoolean(context, expectDFG), kJSPropertyAttributeReadOnly, nullptr);
        JSStringRelease(expected);
        bool passed = semantics(context) && interruptLoop(context);
        JSGlobalContextRelease(context);
        if (!passed)
            return 1;
        std::printf("SCARLET_DFG_PASS stage=vm-teardown iteration=%u\n", iteration);
    }
    std::printf("SCARLET_DFG_PASS stage=complete tier=%s\n", expectDFG ? "DFG" : "Baseline");
    return 0;
}
