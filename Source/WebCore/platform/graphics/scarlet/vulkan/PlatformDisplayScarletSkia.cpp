#include "config.h"
#include "PlatformDisplay.h"
#include <skia/gpu/ganesh/GrDirectContext.h>
namespace WebCore {
static thread_local std::unique_ptr<PlatformDisplay> display;
PlatformDisplay::PlatformDisplay() : m_context(ScarletSkiaVulkan::create())
{
    if (m_context) m_context->grContext()->setResourceCacheLimit(80 * 1024 * 1024);
}
PlatformDisplay& PlatformDisplay::sharedDisplay()
{
    if (!display) display.reset(new PlatformDisplay);
    return *display;
}
PlatformDisplay* PlatformDisplay::sharedDisplayIfExists() { return display.get(); }
void PlatformDisplay::destroySharedDisplay() { display = nullptr; }
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
