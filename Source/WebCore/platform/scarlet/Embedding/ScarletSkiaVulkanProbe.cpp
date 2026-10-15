// Probe-only aggregate. WebCore layer contracts stay above the standalone
// Skia/Vulkan library, which has no dependency on WebCore or its buffers.
#include "config.h"

#if USE(SKIA)
#include "ScarletSkiaVulkan.h"

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedPlatformLayerBufferSkiaImage.h"
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <wtf/MonotonicTime.h>
#include <cstdio>

namespace WebCore {
static int probeVideoPresentationRetry()
{
    auto owner = ScarletSkiaVulkan::create();
    if (!owner)
        return 30;
    auto source = owner->createSurface(16, 16);
    auto target = owner->createSurface(16, 16);
    if (!source || !target)
        return 31;
    source->skSurface().getCanvas()->clear(SK_ColorGREEN);
    if (!owner->submit(*source))
        return 32;
    auto image = source->skSurface().makeImageSnapshot();
    if (!image || !image->isTextureBacked())
        return 33;
    auto buffer = makeUnique<CoordinatedPlatformLayerBufferSkiaImage>(WTF::move(image),
        CoordinatedPlatformLayerBuffer::AlphaMode::Opaque, CoordinatedPlatformLayerBuffer::Rotation::None);
    unsigned notifications = 0;
    bool submitted = false;
    bool notifiedBeforeSubmit = false;
    MonotonicTime notifiedAt;
    buffer->setPresentationCallback([&](MonotonicTime time) {
        ++notifications;
        notifiedBeforeSubmit |= !submitted;
        notifiedAt = time;
    });
    {
        // The failed-submit path discards its wrapper without invoking it.
        // The production buffer must preserve its actual one-shot payload.
        auto failedAttempt = buffer->takePresentationCallback();
        if (!failedAttempt)
            return 34;
    }
    if (notifications)
        return 35;
    auto retry = buffer->takePresentationCallback();
    auto duplicate = buffer->takePresentationCallback();
    if (!retry || !duplicate)
        return 36;
    CoordinatedPlatformLayerBuffer& contents = *buffer;
    target->skSurface().getCanvas()->drawImage(contents.skiaImage(), 0, 0);
    if (!owner->submit(*target))
        return 37;
    submitted = true;
    auto presentationTime = MonotonicTime::now();
    retry(presentationTime);
    duplicate(presentationTime);
    if (auto repeated = buffer->takePresentationCallback())
        repeated(presentationTime);
    if (notifications != 1 || notifiedBeforeSubmit || notifiedAt != presentationTime)
        return 38;

    // Feedback owns its state rather than a raw buffer pointer.
    unsigned retiredNotifications = 0;
    buffer->setPresentationCallback([&](MonotonicTime) { ++retiredNotifications; });
    auto retired = buffer->takePresentationCallback();
    auto retiredDuplicate = buffer->takePresentationCallback();
    buffer = nullptr;
    retired(presentationTime);
    retiredDuplicate(presentationTime);
    if (retiredNotifications != 1)
        return 39;
    std::fprintf(stderr, "SKIA_VULKAN_PASS video presentation feedback discarded-attempt retry duplicate-once retired-buffer real-gpu-submit\n");
    return 0;
}
} // namespace WebCore
#endif // USE(COORDINATED_GRAPHICS)

extern "C" int scarlet_skia_vulkan_probe();

extern "C" int scarlet_webview_vulkan_probe()
{
    if (auto status = scarlet_skia_vulkan_probe())
        return status;
#if USE(COORDINATED_GRAPHICS)
    return WebCore::probeVideoPresentationRetry();
#else
    return 0;
#endif
}
#endif // USE(SKIA)
