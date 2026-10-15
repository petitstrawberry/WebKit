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
#include "ScarletSkiaVulkan.h"
#include "SkiaCompositingLayer.h"
#include "SkiaPaintingEngine.h"
#include "TransformationMatrix.h"
#include <skia/core/SkCanvas.h>
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkSurface.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <cmath>
#include <unistd.h>
#include <wtf/MainThread.h>
#include <wtf/text/WTFString.h>
namespace WebCore {
using namespace WTF::StringLiterals;
ScarletLayerTree::Output::~Output()
{
    for (auto& slot : slots)
        if (slot.frame.handle >= 0) close(slot.frame.handle);
}
void ScarletLayerTree::Output::notify()
{
    std::lock_guard lock(mutex);
    if (live && wake) wake();
}
int ScarletLayerTree::Output::acquire(Frame& out)
{
    std::lock_guard lock(mutex);
    if (failed) return -1;
    if (!live) return 1;
    Slot* latest = nullptr;
    for (auto& slot : slots)
        if (slot.ready && (!latest || slot.frame.generation > latest->frame.generation))
            latest = &slot;
    if (!latest) return 1;
    int handle = dup(latest->frame.handle);
    if (handle < 0) return -1;
    out = latest->frame;
    out.handle = handle;
    latest->ready = false;
    latest->leased = true;
    return 0;
}
int ScarletLayerTree::Output::release(uint64_t generation)
{
    std::lock_guard lock(mutex);
    for (auto& slot : slots) {
        if (slot.leased && slot.frame.generation == generation) {
            slot.leased = false;
            if (wake) wake();
            return 0;
        }
    }
    return 1;
}
int ScarletLayerTree::Output::imageIdentifier(uint64_t generation, uint64_t& out)
{
    std::lock_guard lock(mutex);
    for (const auto& slot : slots)
        if (slot.leased && slot.frame.generation == generation) {
            out = slot.frame.imageIdentifier;
            return 0;
        }
    return 1;
}
unsigned ScarletLayerTree::Output::state() const
{
    std::lock_guard lock(mutex);
    for (const auto& slot : slots) if (slot.ready) return 2;
    return pending ? 1 : 0;
}
unsigned ScarletLayerTree::Output::capacity() const
{
    std::lock_guard lock(mutex);
    if (!live || failed) return 0;
    unsigned n = 0;
    for (const auto& slot : slots) n += !slot.leased && !slot.reserved;
    return n;
}
uint64_t ScarletLayerTree::Output::generation() const
{
    std::lock_guard lock(mutex);
    return nextGeneration;
}
ScarletLayerTree::Output::Statistics ScarletLayerTree::Output::statistics() const
{
    std::lock_guard lock(mutex);
    return counters;
}
struct ScarletLayerTree::Scene {
    Ref<SkiaCompositingLayer> root { SkiaCompositingLayer::create() };
    Vector<Ref<CoordinatedPlatformLayer>> layers;
    struct Slot {
        std::unique_ptr<ScarletSkiaVulkan::Surface> surface;
        std::optional<Damage> damage;
        IntSize size;
    };
    std::array<Slot, 2> slots;
    std::unique_ptr<ScarletSkiaVulkan::Submission> submission;
    Vector<Function<void(MonotonicTime)>> callbacks;
    IntSize size;
    double scale { 1 };
    uint64_t epoch { 0 }, imageIdentifier { 0 }, paintedPixels { 0 };
    unsigned pendingSlot { 0 };
    bool animations { false };
};
std::unique_ptr<ScarletLayerTree> ScarletLayerTree::create(Function<void()>&& schedule)
{
    RELEASE_ASSERT(isMainThread());
    auto tree = std::unique_ptr<ScarletLayerTree>(new ScarletLayerTree(WTF::move(schedule)));
    return tree->m_paintingEngine ? WTF::move(tree) : nullptr;
}
ScarletLayerTree::ScarletLayerTree(Function<void()>&& schedule)
    : m_scheduleRenderingUpdate(WTF::move(schedule))
    , m_ownerRunLoop(RunLoop::currentSingleton())
    , m_workQueue(WorkQueue::create("org.webkit.ScarletCompositor"_s))
    , m_timer(m_workQueue->runLoop(), "ScarletCompositor::Render"_s, this, &ScarletLayerTree::render)
{
    sk_sp<GrContextThreadSafeProxy> proxy;
    m_workQueue->dispatchSync([&] {
        auto& display = PlatformDisplay::sharedDisplay();
        if (!display.prepareSkiaContext()) return;
        proxy = display.skiaGrContext()->threadSafeProxy();
        m_maxTextureSize = display.skiaGrContext()->maxTextureSize();
        m_scene = makeUnique<Scene>();
        m_scene->root->setAnchorPoint({ 0, 0, 0 });
        m_scene->root->setMasksToBounds(true);
        m_scene->root->setDamagePropagationEnabled(true);

    });
    if (proxy) m_paintingEngine = SkiaPaintingEngine::create(proxy);
    m_output->wake = [this, weak = std::weak_ptr<Output>(m_output), queue = m_workQueue.copyRef()] {
        if (m_wakeQueued.exchange(true)) return;
        queue->dispatch([this, weak] {
            if (auto output = weak.lock()) {
                std::lock_guard lock(output->mutex);
                if (output->live) {
                    m_wakeQueued = false;
                    if (!m_detached && !m_timer.isActive())
                        m_timer.startOneShot(0_s);
                }
            }
        });
    };
}
ScarletLayerTree::~ScarletLayerTree() { detach(); }
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
    Vector<Ref<CoordinatedPlatformLayer>> result;
    for (const auto& layer : m_layers) result.append(layer.copyRef());
    return result;
}
void ScarletLayerTree::setRootLayer(GraphicsLayer* layer)
{
    assertIsOwner();
    if (m_detached) { RELEASE_ASSERT(!layer); return; }
    RELEASE_ASSERT(!layer || is<GraphicsLayerCoordinated>(*layer));
    if (m_rootLayer == layer) return;
    m_rootLayer = layer;
    notifyCompositionRequired();
}
void ScarletLayerTree::setOverlayLayer(GraphicsLayer* layer)
{
    assertIsOwner();
    if (m_detached) { RELEASE_ASSERT(!layer); return; }
    m_overlayLayer = layer;
    notifyCompositionRequired();
}
void ScarletLayerTree::setViewport(const IntSize& size, float scale, const IntSize& deviceSize)
{
    assertIsOwner();
    RELEASE_ASSERT(std::isfinite(scale) && scale > 0 && !size.isEmpty());
    if (m_cssViewport == size && m_deviceScale == scale && m_deviceViewport == deviceSize) return;
    std::lock_guard lock(m_commitMutex);
    m_cssViewport = size;
    m_deviceViewport = deviceSize;
    m_deviceScale = scale;
    notifyCompositionRequired();
}
void ScarletLayerTree::setOutputEpoch(uint64_t epoch)
{
    assertIsOwner();
    std::lock_guard lock(m_commitMutex);
    m_epoch = epoch;
}
void ScarletLayerTree::discardOutput()
{
    assertIsOwner();
    m_workQueue->dispatchSync([this] {
        if (!m_scene) return;
        m_scene->submission = nullptr;
        m_scene->callbacks.clear();
        for (auto& slot : m_scene->slots) slot.damage.reset();
        std::lock_guard lock(m_output->mutex);
        m_output->pending = false;
        for (auto& slot : m_output->slots) { slot.ready = false; slot.reserved = false; }
    });
}
void ScarletLayerTree::attachLayer(CoordinatedPlatformLayer& layer)
{
    assertIsOwner();
    layer.setDamagePropagationEnabled(true);
    m_layers.add(Ref { layer });
}
void ScarletLayerTree::detachLayer(CoordinatedPlatformLayer& layer)
{
    assertIsOwner();
    // Keep removed targets alive until the compositor reaches a safe boundary.
    { std::lock_guard lock(m_commitMutex); m_removedLayers.append(Ref { layer }); }
    m_layers.remove(layer);
    if (!m_detached) notifyCompositionRequired();
}
Ref<CoordinatedImageBackingStore> ScarletLayerTree::imageBackingStore(Ref<NativeImage>&& image)
{
    assertIsOwner();
    return CoordinatedImageBackingStore::create(WTF::move(image), m_paintingEngine->threadSafeGrContext());
}
bool ScarletLayerTree::needsComposition() const
{
    return !m_detached && m_ownerCommitRequired;
}
void ScarletLayerTree::notifyCompositionRequired()
{
    assertIsOwner();
    if (m_detached) return;
    m_compositionRequired = true;
    m_ownerCommitRequired = true;
    m_scheduleRenderingUpdate();
}
void ScarletLayerTree::requestComposition(CompositionReason reason)
{
    if (m_detached) return;
    m_compositionRequired = true;
    if (reason == CompositionReason::RenderingUpdate && isMainThread()) {
        m_ownerCommitRequired = true;
        m_scheduleRenderingUpdate();
    }
    m_output->notify();
}
void ScarletLayerTree::willPaintTile() { assertIsOwner(); ++m_pendingTiles; }
void ScarletLayerTree::didPaintTile()
{
    Locker locker { m_paintingLock };
    RELEASE_ASSERT(m_pendingTiles);
    m_pendingTiles.fetch_sub(1, std::memory_order_release);
    m_compositionRequired = true;
    m_output->notify();
    m_paintingCondition.notifyAll();
}
void ScarletLayerTree::didPaintTile(const IntRect& rect)
{
    Locker locker { m_paintingLock };
    ++m_tilePaintStatistics.paints;
    if (!rect.isEmpty()) m_tilePaintStatistics.pixels += static_cast<uint64_t>(rect.width()) * rect.height();
    RELEASE_ASSERT(m_pendingTiles);
    m_pendingTiles.fetch_sub(1, std::memory_order_release);
    m_compositionRequired = true;
    m_output->notify();
    m_paintingCondition.notifyAll();
}
ScarletLayerTree::TilePaintStatistics ScarletLayerTree::tilePaintStatistics() const
{
    Locker locker { m_paintingLock };
    return m_tilePaintStatistics;
}
void ScarletLayerTree::waitUntilPaintingComplete()
{
    for (const auto& layer : layersSnapshot()) layer->waitUntilPaintingComplete();
    Locker locker { m_paintingLock };
    m_paintingCondition.wait(m_paintingLock, [this] { return !m_pendingTiles.load(std::memory_order_acquire); });
}
bool ScarletLayerTree::flush(LocalFrameView& frameView)
{
    assertIsOwner();
    if (m_detached || frameView.needsLayout()) return false;
    auto layers = layersSnapshot();
    for (const auto& layer : layers) layer->flushPendingState();
    Vector<Ref<CoordinatedPlatformLayer>> roots;
    if (m_rootLayer) roots.append(downcast<GraphicsLayerCoordinated>(*m_rootLayer).coordinatedPlatformLayer());
    if (m_overlayLayer) roots.append(downcast<GraphicsLayerCoordinated>(*m_overlayLayer).coordinatedPlatformLayer());
    {
        std::lock_guard lock(m_commitMutex);
        m_pendingLayers = WTF::move(layers);
        m_pendingRoots = WTF::move(roots);
        m_commitPending = true;
    }
    m_compositionRequired = true;
    m_ownerCommitRequired = false;
    m_output->notify();
    return true;
}
void ScarletLayerTree::commit()
{
    // A tile worker publishes complete recordings atomically. While it paints,
    // retain the previous scene and continue video/animation-only composition.
    if (m_pendingTiles.load(std::memory_order_acquire)) return;
    std::lock_guard lock(m_commitMutex);
    if (!m_commitPending) return;
    auto& scene = *m_scene;
    for (const auto& layer : m_removedLayers) layer->invalidateTarget();
    m_removedLayers.clear();
    scene.layers = WTF::move(m_pendingLayers);
    OptionSet<CompositionReason> reasons { CompositionReason::RenderingUpdate, CompositionReason::VideoFrame, CompositionReason::Animation };
    for (const auto& layer : scene.layers) layer->flushPositionChanges(reasons);
    for (const auto& layer : scene.layers) layer->flushCompositingState(reasons);
    for (const auto& layer : scene.layers)
        if (layer->hasPendingBackingStoreTileUpdates()) layer->processPendingBackingStoreTileUpdates();
    Vector<Ref<SkiaCompositingLayer>> children;
    for (const auto& layer : m_pendingRoots) children.append(layer->ensureTarget());
    scene.root->setChildren(WTF::move(children));
    scene.root->setSize(FloatSize(m_cssViewport));
    TransformationMatrix transform;
    transform.scale(m_deviceScale);
    scene.root->setTransform(transform);
    IntSize size = m_deviceViewport;
    if (scene.size != size || scene.scale != m_deviceScale || scene.epoch != m_epoch) {
        for (auto& slot : scene.slots) slot.damage.reset();
        std::lock_guard outputLock(m_output->mutex);
        for (auto& slot : m_output->slots) slot.ready = false;
    }
    scene.size = size;
    scene.scale = m_deviceScale;
    scene.epoch = m_epoch;
    m_commitPending = false;
}
void ScarletLayerTree::render()
{
    RELEASE_ASSERT(!isMainThread());
    if (m_detached || !m_scene) return;
    auto& scene = *m_scene;
    auto device = PlatformDisplay::sharedDisplay().skiaVulkanContext();
    if (!device) return;
    if (scene.submission) {
        auto status = device->submissionStatus(*scene.submission);
        if (status == ScarletSkiaVulkan::SubmissionStatus::Pending) { m_timer.startOneShot(Seconds::fromMilliseconds(1)); return; }
        scene.submission = nullptr;
        if (status == ScarletSkiaVulkan::SubmissionStatus::Failed) {
            std::lock_guard lock(m_output->mutex);
            m_output->failed = true;
            return;
        }
        auto time = MonotonicTime::now();
        auto& target = scene.slots[scene.pendingSlot];
        target.damage.emplace(scene.size);
        {
            std::lock_guard lock(m_output->mutex);
            auto& slot = m_output->slots[scene.pendingSlot];
            // Only the latest unleased completion is retained.
            for (auto& other : m_output->slots) other.ready = false;
            slot.frame.generation = ++m_output->nextGeneration;
            slot.frame.epoch = scene.epoch;
            slot.frame.scale = scene.scale;
            slot.reserved = false;
            slot.ready = true;
            m_output->pending = false;
            ++m_output->counters.paints;
            m_output->counters.pixels += scene.paintedPixels;
        }
        auto callbacks = std::exchange(scene.callbacks, { });
        for (auto& callback : callbacks) callback(time);
    }
    bool required = m_compositionRequired.exchange(false);
    commit();
    if ((!required && !scene.animations) || scene.size.isEmpty()) return;
    auto now = MonotonicTime::now();
    if (now < m_nextFrame) {
        m_compositionRequired = true;
        m_timer.startOneShot(m_nextFrame - now);
        return;
    }
    OptionSet<CompositionReason> reasons { CompositionReason::VideoFrame, CompositionReason::Animation };
    for (const auto& layer : scene.layers) layer->flushCompositingState(reasons);
    unsigned index = 2;
    {
        std::lock_guard lock(m_output->mutex);
        for (unsigned i = 0; i < 2; ++i) {
            auto& slot = m_output->slots[i];
            if (!slot.leased && !slot.ready && !slot.reserved) {
                index = i;
                slot.reserved = true;
                m_output->pending = true;
                break;
            }
        }
    }
    if (index == 2) { m_compositionRequired = true; return; }
    auto& target = scene.slots[index];
    if (!target.surface || target.size != scene.size) {
        target.surface = device->createSurface(scene.size.width(), scene.size.height());
        target.size = scene.size;
        target.damage.reset();
        if (!target.surface) {
            std::lock_guard lock(m_output->mutex);
            m_output->failed = true;
            return;
        }
        auto handle = target.surface->duplicateHandle();
        std::lock_guard lock(m_output->mutex);
        auto& frame = m_output->slots[index].frame;
        if (frame.handle >= 0) close(frame.handle);
        frame = { handle, static_cast<uint32_t>(scene.size.width()), static_cast<uint32_t>(scene.size.height()), 0, ++scene.imageIdentifier, scene.epoch, scene.scale };
    }
    auto prior = target.damage;
    std::optional<Damage> delta { std::in_place, scene.size };
    scene.animations = scene.root->paint(*target.surface->skSurface().getCanvas(), delta, prior, SK_ColorWHITE, &scene.callbacks);
    for (auto& slot : scene.slots) {
        if (!delta) slot.damage.reset();
        else if (slot.damage) slot.damage->add(*delta);
    }
    if (prior && delta && prior->isEmpty() && delta->isEmpty()) {
        scene.callbacks.clear();
        std::lock_guard lock(m_output->mutex);
        m_output->slots[index].reserved = false;
        m_output->pending = false;
        if (scene.animations) m_timer.startOneShot(Seconds { 1.0 / 60 });
        return;
    }
    Damage damage { scene.size, Damage::Mode::Full };
    if (prior && delta) { damage = *prior; damage.add(*delta); }
    scene.paintedPixels = 0;
    for (auto rect : damage.rectsForPainting()) {
        rect.intersect(IntRect { IntPoint { }, scene.size });
        scene.paintedPixels += static_cast<uint64_t>(rect.width()) * rect.height();
    }
    scene.pendingSlot = index;
    m_nextFrame = MonotonicTime::now() + Seconds { 1.0 / 60 };
    scene.submission = device->submitAsync(*target.surface);
    m_timer.startOneShot(Seconds::fromMilliseconds(1));
    if (!scene.submission) {
        std::lock_guard lock(m_output->mutex);
        m_output->failed = true;
    }
}
int ScarletLayerTree::readback(uint64_t generation, Vector<uint8_t>& bytes, uint32_t& width, uint32_t& height)
{
    assertIsOwner();
    int result = 1;
    m_workQueue->dispatchSync([&] {
        unsigned index = 2;
        {
            std::lock_guard lock(m_output->mutex);
            for (unsigned i = 0; i < 2; ++i)
                if (m_output->slots[i].leased && m_output->slots[i].frame.generation == generation) index = i;
        }
        if (index == 2 || !m_scene) return;
        auto& slot = m_scene->slots[index];
        width = slot.size.width(); height = slot.size.height();
        auto info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
        bytes.resize(info.computeMinByteSize());
        result = slot.surface->skSurface().readPixels(info, bytes.mutableSpan().data(), info.minRowBytes(), 0, 0) ? 0 : -1;
    });
    return result;
}
void ScarletLayerTree::detach()
{
    assertIsOwner();
    if (m_detached.exchange(true)) return;
    waitUntilPaintingComplete();
    m_workQueue->dispatchSync([this] {
        m_timer.stop();
        if (m_scene) {
            m_scene->submission = nullptr;
            m_scene->callbacks.clear();
            m_scene->root->setChildren({ });
            for (const auto& layer : m_scene->layers) layer->invalidateTarget();
            for (const auto& layer : m_removedLayers) layer->invalidateTarget();
            m_scene->root->invalidate();
            m_scene = nullptr;
        }
        std::lock_guard lock(m_output->mutex);
        m_output->live = false;
        m_output->wake = nullptr;
        m_output->pending = false;
        for (auto& slot : m_output->slots) slot.ready = false;
    });
    for (const auto& layer : layersSnapshot()) layer->invalidateClient();
    m_layers.clear();
    m_pendingLayers.clear();
    m_pendingRoots.clear();
    m_removedLayers.clear();
    m_rootLayer = nullptr;
    m_overlayLayer = nullptr;
    m_paintingEngine = nullptr;
}
}
#endif
