#include "config.h"
#include "SkiaUtilities.h"
#include "GraphicsTypes.h"
#include "NotImplemented.h"
#include "PlatformDisplay.h"
#include <wtf/MainThread.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>
#include <skia/core/SkImage.h>
#include <skia/gpu/ganesh/SkImageGanesh.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/private/chromium/GrPromiseImageTexture.h>
#include <skia/private/chromium/SkImageChromium.h>
#include <wtf/ThreadSafeWeakPtr.h>
namespace WebCore::SkiaUtilities {
sk_sp<SkImage> rewrapImageForContext(GrDirectContext* context, const SkImage& image)
{
    if (!context || !image.isValid(context->asRecorder())) return nullptr;
    GrBackendTexture texture;
    if (!SkImages::GetBackendTextureFromImage(&image, &texture, false)) return nullptr;
    return SkImages::BorrowTextureFrom(context, texture, kTopLeft_GrSurfaceOrigin, image.colorType(), image.alphaType(), image.refColorSpace());
}
std::unique_ptr<SkiaGpuFence> flushAndSubmitSurfaceWithFence(GrDirectContext* context, SkSurface* surface)
{
    context->flush(surface); context->submit(GrSyncCpu::kYes);
    return nullptr; // GPU completion, not an emulated asynchronous fence.
}
std::unique_ptr<SkiaGpuFence> flushAndSubmitImageWithFence(GrDirectContext* context, const sk_sp<SkImage>& image)
{
    context->flush(image); context->submit(GrSyncCpu::kYes); return nullptr;
}
std::unique_ptr<SkiaGpuFence> flushAndSubmitWithFence(GrDirectContext* context)
{
    context->flush(); context->submit(GrSyncCpu::kYes); return nullptr;
}
// Resolve producers on the owner before handing immutable promises to CPU-only
// DDL recorders. Fulfillment uses that same owner/context and Vulkan stream.
class NativePromiseImageContext {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(NativePromiseImageContext);
public:
    NativePromiseImageContext(const sk_sp<SkImage>& image, GrBackendTexture&& texture)
        : m_image(image), m_texture(WTF::move(texture)), m_owner(RunLoop::currentSingleton()) { }
    ~NativePromiseImageContext()
    {
        if (auto owner = m_owner.get(); owner && owner.get() != &RunLoop::currentSingleton()) {
            // Never synchronously dispatch: the owner may be joining painting.
            owner->dispatch([image = WTF::move(m_image)]() mutable { image = nullptr; });
        }
    }
    sk_sp<GrPromiseImageTexture> fulfill()
    {
        return GrPromiseImageTexture::Make(m_texture);
    }
private:
    sk_sp<SkImage> m_image;
    GrBackendTexture m_texture;
    ThreadSafeWeakPtr<RunLoop> m_owner;
};

sk_sp<SkImage> createPromiseImageIfNeeded(const sk_sp<SkImage>& image, const sk_sp<GrContextThreadSafeProxy>& proxy)
{
    if (!image->isTextureBacked())
        return image;
    auto* context = PlatformDisplay::sharedDisplay().skiaGrContext();
    if (!context || !proxy || !image->isValid(context->asRecorder()))
        return image;
    GrBackendTexture texture;
    GrSurfaceOrigin origin;
    // Flush the producer proxy without submitting or waiting for the GPU. The
    // later owner DrawDDL is encoded after these writes in the same stream.
    if (!SkImages::GetBackendTextureFromImage(image.get(), &texture, true, &origin))
        return image; // GraphicsContextSkia marks this as owner-only replay.
    // Different contexts share a device, but not a Ganesh command stream.
    // Complete producer writes before publishing the immutable backend texture.
    if (context->threadSafeProxy() != proxy && !context->submit(GrSyncCpu::kYes))
        return image;
    auto backendFormat = texture.getBackendFormat();
    auto mipmapped = texture.mipmapped();
    auto payload = makeUnique<NativePromiseImageContext>(image, WTF::move(texture));
    auto promised = SkImages::PromiseTextureFrom(proxy, backendFormat, image->dimensions(), mipmapped,
        origin, image->colorType(), image->alphaType(), image->refColorSpace(),
        +[](void* opaque) -> sk_sp<GrPromiseImageTexture> { return static_cast<NativePromiseImageContext*>(opaque)->fulfill(); },
        +[](void* opaque) { delete static_cast<NativePromiseImageContext*>(opaque); }, payload.release());
    if (!promised)
        return image;
    return promised;
}

SkBlendMode toSkiaBlendMode(BlendMode blendMode, std::optional<CompositeOperator> operation)
{
    switch (blendMode) {
    case BlendMode::Normal:
        switch (operation.value_or(CompositeOperator::SourceOver)) {
        case CompositeOperator::Clear:
            return SkBlendMode::kClear;
        case CompositeOperator::Copy:
            return SkBlendMode::kSrc;
        case CompositeOperator::SourceOver:
            return SkBlendMode::kSrcOver;
        case CompositeOperator::SourceIn:
            return SkBlendMode::kSrcIn;
        case CompositeOperator::SourceOut:
            return SkBlendMode::kSrcOut;
        case CompositeOperator::SourceAtop:
            return SkBlendMode::kSrcATop;
        case CompositeOperator::DestinationOver:
            return SkBlendMode::kDstOver;
        case CompositeOperator::DestinationIn:
            return SkBlendMode::kDstIn;
        case CompositeOperator::DestinationOut:
            return SkBlendMode::kDstOut;
        case CompositeOperator::DestinationAtop:
            return SkBlendMode::kDstATop;
        case CompositeOperator::XOR:
            return SkBlendMode::kXor;
        case CompositeOperator::PlusLighter:
            return SkBlendMode::kPlus;
        case CompositeOperator::PlusDarker:
            notImplemented();
            return SkBlendMode::kSrcOver;
        case CompositeOperator::Difference:
            return SkBlendMode::kDifference;
        }
        break;
    case BlendMode::Multiply:
        return SkBlendMode::kMultiply;
    case BlendMode::Screen:
        return SkBlendMode::kScreen;
    case BlendMode::Overlay:
        return SkBlendMode::kOverlay;
    case BlendMode::Darken:
        return SkBlendMode::kDarken;
    case BlendMode::Lighten:
        return SkBlendMode::kLighten;
    case BlendMode::ColorDodge:
        return SkBlendMode::kColorDodge;
    case BlendMode::ColorBurn:
        return SkBlendMode::kColorBurn;
    case BlendMode::HardLight:
        return SkBlendMode::kHardLight;
    case BlendMode::SoftLight:
        return SkBlendMode::kSoftLight;
    case BlendMode::Difference:
        return SkBlendMode::kDifference;
    case BlendMode::Exclusion:
        return SkBlendMode::kExclusion;
    case BlendMode::Hue:
        return SkBlendMode::kHue;
    case BlendMode::Saturation:
        return SkBlendMode::kSaturation;
    case BlendMode::Color:
        return SkBlendMode::kColor;
    case BlendMode::Luminosity:
        return SkBlendMode::kLuminosity;
    case BlendMode::PlusLighter:
        return SkBlendMode::kPlus;
    case BlendMode::PlusDarker:
        notImplemented();
        break;
    }

    return SkBlendMode::kSrcOver;
}

}
