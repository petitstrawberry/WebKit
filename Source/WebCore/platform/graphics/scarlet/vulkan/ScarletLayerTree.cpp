/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "config.h"
#include "ScarletLayerTree.h"
#if PLATFORM(SCARLET) && USE(COORDINATED_GRAPHICS)
#include "CoordinatedImageBackingStore.h"
#include "GraphicsLayerCoordinated.h"
#include "LocalFrameView.h"
#include "NativeImage.h"
#include "PlatformDisplay.h"
#include "SkiaPaintingEngine.h"
#include "TransformationMatrix.h"
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <wtf/MainThread.h>
namespace WebCore {
std::unique_ptr<ScarletLayerTree> ScarletLayerTree::create(Function<void()>&& schedule)
{
    RELEASE_ASSERT(isMainThread());
    if (!PlatformDisplay::sharedDisplay().prepareSkiaContext())
        return nullptr;
    return std::unique_ptr<ScarletLayerTree>(new ScarletLayerTree(WTF::move(schedule)));
}
ScarletLayerTree::ScarletLayerTree(Function<void()>&& schedule)
    : m_scheduleRenderingUpdate(WTF::move(schedule))
    , m_ownerRunLoop(RunLoop::currentSingleton())
    , m_paintingEngine(SkiaPaintingEngine::create(PlatformDisplay::sharedDisplay().skiaGrContext()->threadSafeProxy()))
    , m_sceneRoot(SkiaCompositingLayer::create())
{
    RELEASE_ASSERT(m_paintingEngine);
    m_sceneRoot->setAnchorPoint({ 0, 0, 0 });
    m_sceneRoot->setMasksToBounds(true);
    m_sceneRoot->setDamagePropagationEnabled(true);
}
ScarletLayerTree::~ScarletLayerTree()
{
    detach();
}
void ScarletLayerTree::assertIsOwner() const
{
    RELEASE_ASSERT(isMainThread() && &RunLoop::currentSingleton() == &m_ownerRunLoop);
}
Ref<GraphicsLayer> ScarletLayerTree::createGraphicsLayer(GraphicsLayer::Type type, GraphicsLayerClient& client)
{
    assertIsOwner();
    RELEASE_ASSERT(!m_detached);
    return adoptRef(*new GraphicsLayerCoordinated(type, client, CoordinatedPlatformLayer::create(*this)));
}
Vector<Ref<CoordinatedPlatformLayer>> ScarletLayerTree::layersSnapshot() const
{
    Vector<Ref<CoordinatedPlatformLayer>> layers;
    layers.reserveInitialCapacity(m_layers.size());
    for (const auto& layer : m_layers)
        layers.append(layer.copyRef());
    return layers;
}
void ScarletLayerTree::setRootLayer(GraphicsLayer* layer)
{
    assertIsOwner();
    if (m_detached) {
        // Page teardown may send its final detach notification after detach().
        RELEASE_ASSERT(!layer);
        return;
    }
    RELEASE_ASSERT(!layer || is<GraphicsLayerCoordinated>(*layer));
    if (m_rootLayer == layer)
        return;
    m_rootLayer = layer;
    notifyCompositionRequired();
}
void ScarletLayerTree::setOverlayLayer(GraphicsLayer* layer)
{
    assertIsOwner();
    if (m_detached) {
        // Page teardown may send its final detach notification after detach().
        RELEASE_ASSERT(!layer);
        return;
    }
    RELEASE_ASSERT(!layer || is<GraphicsLayerCoordinated>(*layer));
    m_overlayLayer = layer;
    notifyCompositionRequired();
}
void ScarletLayerTree::setViewport(const IntSize& size, float scale)
{
    assertIsOwner();
    RELEASE_ASSERT(std::isfinite(scale) && scale > 0 && !size.isEmpty());
    if (m_cssViewport == size && m_deviceScale == scale)
        return;
    m_cssViewport = size;
    m_deviceScale = scale;
    m_sceneRoot->setSize(FloatSize(size));
    TransformationMatrix transform;
    transform.scale(scale);
    m_sceneRoot->setTransform(transform);
    notifyCompositionRequired();
}
void ScarletLayerTree::attachLayer(CoordinatedPlatformLayer& layer)
{
    assertIsOwner();
    RELEASE_ASSERT(!m_detached);
    layer.setDamagePropagationEnabled(true);
    m_layers.add(Ref { layer });
}
void ScarletLayerTree::detachLayer(CoordinatedPlatformLayer& layer)
{
    assertIsOwner();
    waitUntilPaintingComplete();
    layer.invalidateTarget();
    m_layers.remove(layer);
    if (!m_detached)
        notifyCompositionRequired();
}
Ref<CoordinatedImageBackingStore> ScarletLayerTree::imageBackingStore(Ref<NativeImage>&& image)
{
    assertIsOwner();
    return CoordinatedImageBackingStore::create(WTF::move(image), m_paintingEngine->threadSafeGrContext());
}
bool ScarletLayerTree::needsComposition() const
{
    assertIsOwner();
    if (m_detached)
        return false;
    if (m_compositionRequired || m_runningAnimations || m_pendingTiles)
        return true;
    for (const auto& layer : m_layers) {
        if (layer->hasPendingTilesCreation())
            return true;
    }
    return false;
}
void ScarletLayerTree::notifyCompositionRequired()
{
    assertIsOwner();
    if (m_detached)
        return;
    m_compositionRequired = true;
    m_scheduleRenderingUpdate();
}
void ScarletLayerTree::requestComposition(CompositionReason reason)
{
    assertIsOwner();
    if (m_detached)
        return;
    m_compositionRequired = true;
    // Video and compositor animations update retained scene state without
    // requesting a Page rendering update or running DOM rendering steps.
    if (reason == CompositionReason::RenderingUpdate)
        m_scheduleRenderingUpdate();
}
int ScarletLayerTree::maxTextureSize() const
{
    assertIsOwner();
    auto* context = PlatformDisplay::sharedDisplay().skiaGrContext();
    RELEASE_ASSERT(context && !context->abandoned());
    return context->maxTextureSize();
}
void ScarletLayerTree::willPaintTile()
{
    assertIsOwner();
    ++m_pendingTiles;
}
void ScarletLayerTree::didPaintTile()
{
    // Painting workers publish only completion. GPU/scene access stays owner-only.
    Locker locker { m_paintingLock };
    RELEASE_ASSERT(m_pendingTiles.load(std::memory_order_relaxed));
    m_pendingTiles.fetch_sub(1, std::memory_order_release);
    m_paintingCondition.notifyAll();
}
void ScarletLayerTree::didPaintTile(const IntRect& rect)
{
    Locker locker { m_paintingLock };
    ++m_tilePaintStatistics.paints;
    if (!rect.isEmpty())
        m_tilePaintStatistics.pixels += static_cast<uint64_t>(rect.width()) * rect.height();
    RELEASE_ASSERT(m_pendingTiles.load(std::memory_order_relaxed));
    m_pendingTiles.fetch_sub(1, std::memory_order_release);
    m_paintingCondition.notifyAll();
}
ScarletLayerTree::TilePaintStatistics ScarletLayerTree::tilePaintStatistics() const
{
    assertIsOwner();
    Locker locker { m_paintingLock };
    return m_tilePaintStatistics;
}
void ScarletLayerTree::updateSceneRoot()
{
    Vector<Ref<SkiaCompositingLayer>> children;
    if (m_rootLayer)
        children.append(downcast<GraphicsLayerCoordinated>(*m_rootLayer).coordinatedPlatformLayer().ensureTarget());
    if (m_overlayLayer)
        children.append(downcast<GraphicsLayerCoordinated>(*m_overlayLayer).coordinatedPlatformLayer().ensureTarget());
    m_sceneRoot->setChildren(WTF::move(children));
}
void ScarletLayerTree::waitUntilPaintingComplete()
{
    assertIsOwner();
    // Use CoordinatedGraphics' existing per-buffer publication barrier before
    // taking pending updates. The callback follows buffer completion, so join
    // the pending counter too before releasing clients or asserting settled.
    for (const auto& layer : layersSnapshot())
        layer->waitUntilPaintingComplete();
    Locker locker { m_paintingLock };
    m_paintingCondition.wait(m_paintingLock, [this] {
        return !m_pendingTiles.load(std::memory_order_acquire);
    });
}

bool ScarletLayerTree::flush(LocalFrameView& frameView)
{
    assertIsOwner();
    if (m_detached || frameView.needsLayout())
        return false;
    auto layers = layersSnapshot();
    for (const auto& layer : layers)
        layer->flushPendingState();
    OptionSet<CompositionReason> reasons { CompositionReason::RenderingUpdate, CompositionReason::VideoFrame, CompositionReason::Animation };
    for (const auto& layer : layers)
        layer->flushPositionChanges(reasons);
    waitUntilPaintingComplete();
    for (const auto& layer : layers)
        layer->flushCompositingState(reasons);
    for (const auto& layer : layers) {
        if (layer->hasPendingBackingStoreTileUpdates())
            layer->processPendingBackingStoreTileUpdates();
    }
    RELEASE_ASSERT(!m_pendingTiles);
    updateSceneRoot();
    return true;
}
bool ScarletLayerTree::paint(SkCanvas& canvas, std::optional<Damage>& frameDamage,
    const std::optional<Damage>& priorTargetDamage, std::optional<SkColor> clearColor)
{
    assertIsOwner();
    RELEASE_ASSERT(!m_detached);
    static const bool profileEnabled = [] {
        const char* value = std::getenv("SCARLET_WEBVIEW_PROFILE");
        return value && !std::strcmp(value, "1");
    }();
    auto profileStart = profileEnabled ? MonotonicTime::now() : MonotonicTime { };
    RELEASE_ASSERT(PlatformDisplay::sharedDisplay().prepareSkiaContext());
    if (profileEnabled) {
        auto end = MonotonicTime::now();
        static thread_local MonotonicTime periodStart;
        static thread_local uint64_t paints { 0 }, prepareMicros { 0 };
        if (!periodStart)
            periodStart = profileStart;
        ++paints;
        prepareMicros += static_cast<uint64_t>(std::max(0.0, (end - profileStart).microseconds()));
        if ((end - periodStart).seconds() >= 1) {
            std::fprintf(stderr, "SCARLET_SCENE_CONTEXT_PROFILE tree=%p period_us=%llu paints=%llu prepare_us=%llu\n",
                static_cast<void*>(this), static_cast<unsigned long long>((end - periodStart).microseconds()),
                static_cast<unsigned long long>(paints), static_cast<unsigned long long>(prepareMicros));
            periodStart = end;
            paints = prepareMicros = 0;
        }
    }
    m_presentationCallbacks.clear();
    m_compositionRequired = false;
    m_runningAnimations = m_sceneRoot->paint(canvas, frameDamage, priorTargetDamage, clearColor, &m_presentationCallbacks);
    // needsComposition() keeps animated scene frames scheduled.
    return m_runningAnimations;
}
void ScarletLayerTree::didSubmit(MonotonicTime time)
{
    assertIsOwner();
    auto callbacks = std::exchange(m_presentationCallbacks, { });
    for (auto& callback : callbacks)
        callback(time);
}
void ScarletLayerTree::discardPendingPresentationCallbacks()
{
    assertIsOwner();
    m_presentationCallbacks.clear();
}
void ScarletLayerTree::detach()
{
    assertIsOwner();
    if (std::exchange(m_detached, true))
        return;
    waitUntilPaintingComplete();
    m_runningAnimations = false;
    m_compositionRequired = false;
    m_presentationCallbacks.clear();
    m_sceneRoot->setChildren({ });
    m_rootLayer = nullptr;
    m_overlayLayer = nullptr;
    auto layers = layersSnapshot();
    for (const auto& layer : layers) {
        layer->invalidateTarget();
        layer->invalidateClient();
    }
    m_layers.clear();
    m_sceneRoot->invalidate();
}
} // namespace WebCore
#endif
