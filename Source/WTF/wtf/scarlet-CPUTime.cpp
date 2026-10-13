// Experimental native Scarlet port. No POSIX resource accounting emulation.
#include "config.h"
#include <wtf/CPUTime.h>
#include <wtf/scarlet/NativeThreadGC.h>
#include <vector>
namespace WTF {
struct NativeTaskInfo {
    uint64_t pid, ppid; uint8_t state,type,cpu,reserved; int32_t exitStatus;
    uint64_t tgid; char name[64]; uint64_t cpuNs;
    uint32_t util,min,capacity; uint8_t preference,padding[3];
    uint64_t migrations; int32_t nice; uint32_t weight; uint64_t vruntime,deadline;
};
static_assert(sizeof(NativeTaskInfo)==152);
static_assert(offsetof(NativeTaskInfo,cpuNs)==96);
std::optional<CPUTime> CPUTime::get()
{
    // Scarlet accounts total per-task CPU time, without a user/kernel split.
    // Report absence rather than manufacture a split for this optional metric.
    return std::nullopt;
}
Seconds CPUTime::forCurrentThread()
{
    auto count=scarlet_native_call(24,0,0,0,0,0);
    RELEASE_ASSERT(count != ~0ul && count < 100000);
    std::vector<NativeTaskInfo> tasks(count+32);
    auto n=scarlet_native_call(25,reinterpret_cast<uintptr_t>(tasks.data()),tasks.size(),0,0,0);
    RELEASE_ASSERT(n != ~0ul && n <= tasks.size());
    auto id=scarlet_native_call(7,0,0,0,0,0);
    for(size_t i=0;i<n;++i)if(tasks[i].pid==id)return Seconds::fromNanoseconds(tasks[i].cpuNs);
    RELEASE_ASSERT_NOT_REACHED();
}
}
