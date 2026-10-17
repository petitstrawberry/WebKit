#include "config.h"
#include "PlatformDisplay.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/ThreadSpecific.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
namespace WebCore {
static ThreadSpecific<std::unique_ptr<PlatformDisplay>>& threadDisplay()
{
    // Use WebKit's pthread-backed storage and thread-exit destructor.
    static NeverDestroyed<ThreadSpecific<std::unique_ptr<PlatformDisplay>>> display;
    return display.get();
}
PlatformDisplay::PlatformDisplay() : m_context(ScarletSkiaVulkan::create())
{
    // ScarletSkiaVulkan shares one cache budget across the device's contexts.
    // Each context applies its current share on its own thread before use.
}
PlatformDisplay& PlatformDisplay::sharedDisplay()
{
    auto& display = threadDisplay();
    if (!*display) display->reset(new PlatformDisplay);
    return **display;
}
PlatformDisplay* PlatformDisplay::sharedDisplayIfExists()
{
    auto& display = threadDisplay();
    return display.isSet() ? display->get() : nullptr;
}
void PlatformDisplay::destroySharedDisplay()
{
    auto& display = threadDisplay();
    if (display.isSet()) display->reset();
}
GrDirectContext* PlatformDisplay::skiaGrContext() const { return m_context ? m_context->grContext() : nullptr; }
bool PlatformDisplay::prepareSkiaContext() const { auto* context = skiaGrContext(); return context && !context->abandoned(); }
size_t PlatformDisplay::maxSkiaResourceCacheBytes() const { auto* context = skiaGrContext(); return context ? context->getResourceCacheLimit() : 0; }
void PlatformDisplay::skiaReleaseUnusedResources(WTF::Critical critical)
{
    auto* context = skiaGrContext();
    if (!context || context->abandoned()) return;
    if (critical == WTF::Critical::Yes) context->freeGpuResources();
    else context->purgeUnlockedResources(GrPurgeResourceOptions::kScratchResourcesOnly);
}
}
