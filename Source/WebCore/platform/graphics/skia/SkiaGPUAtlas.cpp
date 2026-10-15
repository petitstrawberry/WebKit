/*
 * Copyright (C) 2025, 2026 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SkiaGPUAtlas.h"

#if USE(SKIA)
#if PLATFORM(SCARLET)

#include "PlatformDisplay.h"
#include "SkiaImageAtlasLayout.h"
#include "SkiaUtilities.h"
#include <skia/core/SkCanvas.h>
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkPaint.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <wtf/MainThread.h>
#include <wtf/TZoneMallocInlines.h>
namespace WebCore {
WTF_MAKE_TZONE_ALLOCATED_IMPL(SkiaGPUAtlas);
SkiaGPUAtlas::SkiaGPUAtlas(sk_sp<SkSurface>&& surface, Ref<AtlasUploadCondition>&& condition, const sk_sp<GrContextThreadSafeProxy>& proxy, const SkiaImageAtlasLayout& layout, const IntSize& size)
    : m_surface(WTF::move(surface))
    , m_uploadCondition(WTF::move(condition))
    , m_threadSafeGrContext(proxy)
    , m_layout(layout)
    , m_size(size)
{
    for (const auto& entry : layout.entries()) {
        const auto& rect = entry.atlasRect;
        m_imageToRect.add(entry.rasterImage.get(), SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()));
    }
}
SkiaGPUAtlas::~SkiaGPUAtlas() = default;
RefPtr<SkiaGPUAtlas> SkiaGPUAtlas::create(const SkiaImageAtlasLayout& layout, Ref<AtlasUploadCondition>&& condition, const sk_sp<GrContextThreadSafeProxy>& proxy)
{
    RELEASE_ASSERT(isMainThread());
    auto& display = PlatformDisplay::sharedDisplay();
    if (!display.prepareSkiaContext() || layout.atlasSize().isEmpty())
        return nullptr;
    auto* context = display.skiaGrContext();
    RELEASE_ASSERT(context->threadSafeProxy() == proxy);
    const auto& size = layout.atlasSize();
    auto info = SkImageInfo::Make(size.width(), size.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::RenderTarget(context, skgpu::Budgeted::kYes, info);
    if (!surface)
        return nullptr;
    return adoptRef(*new SkiaGPUAtlas(WTF::move(surface), WTF::move(condition), proxy, layout, size));
}
void SkiaGPUAtlas::uploadImages()
{
    RELEASE_ASSERT(isMainThread());
    auto* canvas = m_surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    for (const auto& entry : m_layout->entries())
        canvas->drawImage(entry.rasterImage, entry.atlasRect.x(), entry.atlasRect.y(), SkSamplingOptions(), &paint);
    m_image = m_surface->makeImageSnapshot();
    RELEASE_ASSERT(m_image && m_image->isTextureBacked());
    m_workerImage = SkiaUtilities::createPromiseImageIfNeeded(m_image, m_threadSafeGrContext);
    RELEASE_ASSERT(m_workerImage);
}
void SkiaGPUAtlas::waitForUpload() const
{
    // Upload and consumption share the owner Ganesh command stream.
    RELEASE_ASSERT(isMainThread() && m_image);
}
sk_sp<SkImage> SkiaGPUAtlas::atlasImageForCurrentThread() const
{
    // Prepared once on the owner before publication to the painting workers.
    // Workers copy only the immutable promise; fulfillment remains on owner.
    RELEASE_ASSERT(m_workerImage && (isMainThread() || canReplayOnWorker()));
    return m_workerImage;
}
} // namespace WebCore

#else

#include "BitmapTexture.h"
#include "GLContext.h"
#include "PlatformDisplay.h"
#include "SkiaImageAtlasLayout.h"
#if USE(GBM)
#include "MemoryMappedGPUBuffer.h"
#endif
#include <wtf/TZoneMallocInlines.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkPixmap.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/SkImageGanesh.h>
#include <skia/private/chromium/GrPromiseImageTexture.h>
#include <skia/private/chromium/SkImageChromium.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SkiaGPUAtlas);

SkiaGPUAtlas::SkiaGPUAtlas(Ref<BitmapTexture>&& atlasTexture, GrBackendTexture&& backendTexture, Ref<AtlasUploadCondition>&& uploadCondition, const sk_sp<GrContextThreadSafeProxy>& threadSafeGrContext, const SkiaImageAtlasLayout& layout, const IntSize& size)
    : m_atlasTexture(WTF::move(atlasTexture))
    , m_backendTexture(WTF::move(backendTexture))
    , m_uploadCondition(WTF::move(uploadCondition))
    , m_threadSafeGrContext(threadSafeGrContext)
    , m_layout(layout)
    , m_size(size)
{
    m_imageToRect.reserveInitialCapacity(layout.entries().size());
    for (const auto& entry : layout.entries()) {
        const auto& rect = entry.atlasRect;
        m_imageToRect.add(entry.rasterImage.get(), SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()));
    }
}

SkiaGPUAtlas::~SkiaGPUAtlas() = default;

RefPtr<SkiaGPUAtlas> SkiaGPUAtlas::create(const SkiaImageAtlasLayout& layout, Ref<BitmapTexture>&& atlasTexture, Ref<AtlasUploadCondition>&& uploadCondition, const sk_sp<GrContextThreadSafeProxy>& threadSafeGrContext)
{
    const auto& atlasSize = layout.atlasSize();
    if (atlasSize.isEmpty())
        return nullptr;

    RELEASE_ASSERT(atlasSize == atlasTexture->size());

    auto backendTexture = atlasTexture->createSkiaBackendTexture();
    if (!backendTexture.isValid())
        return nullptr;

    return adoptRef(*new SkiaGPUAtlas(WTF::move(atlasTexture), WTF::move(backendTexture), WTF::move(uploadCondition), threadSafeGrContext, layout, atlasSize));
}

void SkiaGPUAtlas::uploadImages()
{
    Vector<uint8_t> conversionBuffer;

    // Returns pixel data for atlas upload — either a zero-copy reference to the original
    // pixels (fast path) or a converted sRGB copy (for non-sRGB images).
    auto pixelDataInSRGB = [&conversionBuffer](const sk_sp<SkImage>& image) -> std::optional<std::pair<const void*, size_t>> {
        SkPixmap pixmap;
        if (!image->peekPixels(&pixmap))
            return std::nullopt;

        if (auto* colorSpace = image->colorSpace(); !colorSpace || colorSpace->isSRGB())
            return std::pair { pixmap.addr(), pixmap.rowBytes() };

        // Convert to sRGB using Skia's built-in color space conversion.
        auto srgbInfo = SkImageInfo::Make(image->width(), image->height(), image->colorType(), image->alphaType(), SkColorSpace::MakeSRGB());
        size_t srgbRowBytes = srgbInfo.minRowBytes();
        conversionBuffer.resize(srgbInfo.computeMinByteSize());

        if (!image->readPixels(static_cast<GrDirectContext*>(nullptr), srgbInfo, conversionBuffer.mutableSpan().data(), srgbRowBytes, 0, 0))
            return std::nullopt;

        return std::pair { static_cast<const void*>(conversionBuffer.mutableSpan().data()), srgbRowBytes };
    };

#if USE(GBM)
    if (auto* gpuBuffer = m_atlasTexture->memoryMappedGPUBuffer()) {
        RELEASE_ASSERT(gpuBuffer->isLinear() || gpuBuffer->isVivanteSuperTiled());
        {
            auto writeScope = makeGPUBufferWriteScope(*gpuBuffer);
            RELEASE_ASSERT_WITH_MESSAGE(writeScope, "Failed to map GPU buffer for atlas upload");

            for (const auto& entry : m_layout->entries()) {
                if (auto pixels = pixelDataInSRGB(entry.rasterImage))
                    gpuBuffer->updateContents(*writeScope, pixels->first, entry.atlasRect, pixels->second);
            }
        }

        m_uploadCondition->signal();
        return;
    }
#endif

    // GL fallback: use BitmapTexture::updateContents() per entry.
    for (const auto& entry : m_layout->entries()) {
        if (auto pixels = pixelDataInSRGB(entry.rasterImage))
            m_atlasTexture->updateContents(pixels->first, entry.atlasRect, IntPoint::zero(), pixels->second, PixelFormat::BGRA8);
    }

    m_uploadFence = GLFence::create(PlatformDisplay::sharedDisplay().glDisplay());
}

void SkiaGPUAtlas::waitForUpload() const
{
    if (m_uploadFence) {
        m_uploadFence->serverWait();
        return;
    }

    m_uploadCondition->wait();
}

sk_sp<SkImage> SkiaGPUAtlas::atlasImageForCurrentThread() const
{
    if (m_threadSafeGrContext) {
        ref();
        auto backendFormat = m_threadSafeGrContext->defaultBackendFormat(kBGRA_8888_SkColorType, GrRenderable::kYes);
        ASSERT(backendFormat.isValid());
        return SkImages::PromiseTextureFrom(m_threadSafeGrContext, backendFormat, SkISize::Make(m_atlasTexture->size().width(), m_atlasTexture->size().height()), skgpu::Mipmapped::kNo,
            kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB(),
            +[](void* userData) -> sk_sp<GrPromiseImageTexture> {
                auto& atlas = *static_cast<SkiaGPUAtlas*>(userData);
                atlas.waitForUpload();
                return GrPromiseImageTexture::Make(atlas.m_backendTexture);
            },
            +[](void* userData) {
                static_cast<SkiaGPUAtlas*>(userData)->deref();
            }, const_cast<SkiaGPUAtlas*>(this));
    }

    auto* glContext = PlatformDisplay::sharedDisplay().skiaGLContext();
    if (!glContext || !glContext->makeContextCurrent())
        return nullptr;

    waitForUpload();

    auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
    ASSERT(grContext);
    ASSERT(m_backendTexture.isValid());
    return SkImages::BorrowTextureFrom(grContext, m_backendTexture, kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
}

} // namespace WebCore


#endif // PLATFORM(SCARLET)
#endif // USE(SKIA)
