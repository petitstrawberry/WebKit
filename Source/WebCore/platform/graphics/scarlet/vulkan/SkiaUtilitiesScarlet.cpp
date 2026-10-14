#include "config.h"
#include "SkiaUtilities.h"
#include "GraphicsTypes.h"
#include "NotImplemented.h"
#include "PlatformDisplay.h"
#include <skia/core/SkImage.h>
#include <skia/gpu/ganesh/SkImageGanesh.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
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
sk_sp<SkImage> createPromiseImageIfNeeded(const sk_sp<SkImage>& image, const sk_sp<GrContextThreadSafeProxy>& proxy)
{
    if (!image->isTextureBacked()) return image;
    auto* context = PlatformDisplay::sharedDisplay().skiaGrContext();
    if (!context || !proxy || !image->isValid(context->asRecorder()) || context->threadSafeProxy() != proxy) return nullptr;
    return image;
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
