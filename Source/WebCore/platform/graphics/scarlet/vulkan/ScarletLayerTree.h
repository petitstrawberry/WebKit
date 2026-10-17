/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#if PLATFORM(SCARLET) && USE(COORDINATED_GRAPHICS)
#include "CoordinatedPlatformLayer.h"
#include "Damage.h"
#include "GraphicsLayerFactory.h"
#include "GraphicsLayer.h"
#include "IntSize.h"
#include "IntRect.h"
#include <array>
#include <atomic>
#include <memory>
#include <functional>
#include <mutex>
#include <wtf/Condition.h>
#include <wtf/HashSet.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/WorkQueue.h>
namespace WebCore {
class LocalFrameView;
class SkiaPaintingEngine;

// GraphicsLayers and DOM stay on the owner. The committed scene, Ganesh,
// surfaces and fences belong exclusively to the compositing run loop.
class ScarletLayerTree final : public GraphicsLayerFactory, public CoordinatedPlatformLayer::Client {
public:
    struct Output {
        // Keep a render target available while the consumer holds its current
        // image and the next image staged for presentation.
        static constexpr unsigned slotCount = 3;
        struct Frame {
            int32_t handle { -1 };
            uint32_t width { 0 }, height { 0 };
            uint64_t generation { 0 }, imageIdentifier { 0 }, epoch { 0 };
            double scale { 1 };
            IntRect damage;
        };
        ~Output();
        int acquire(Frame&);
        int release(uint64_t);
        int imageIdentifier(uint64_t, uint64_t&);
        int frameDamage(uint64_t, IntRect&);
        unsigned state() const;
        unsigned capacity() const;
        uint64_t generation() const;
        struct Statistics { uint64_t paints { 0 }, pixels { 0 }; };
        Statistics statistics() const;
    private:
        friend class ScarletLayerTree;
        struct Slot { Frame frame; bool leased { false }, ready { false }, reserved { false }; };
        mutable std::mutex mutex;
        std::array<Slot, slotCount> slots;
        bool live { true }, failed { false }, pending { false };
        uint64_t nextGeneration { 0 };
        Statistics counters;
        IntRect unacquiredDamage;
        std::function<void()> wake;
        void notify();
    };
    static std::unique_ptr<ScarletLayerTree> create(Function<void()>&& scheduleRenderingUpdate);
    ~ScarletLayerTree();
    GraphicsLayerFactory* factory() { return this; }
    void setRootLayer(GraphicsLayer*);
    void setOverlayLayer(GraphicsLayer*);
    int maximumTextureSize() const { return m_maxTextureSize; }
    void setViewport(const IntSize&, float, const IntSize& deviceSize);
    void setOutputEpoch(uint64_t);
    void discardOutput();
    std::shared_ptr<Output> output() const { return m_output; }
    bool flush(LocalFrameView&);
    struct TilePaintStatistics { uint64_t paints { 0 }, pixels { 0 }; };
    TilePaintStatistics tilePaintStatistics() const;
    bool needsComposition() const;
    bool hasPendingComposition() const;
    int readback(uint64_t, Vector<uint8_t>&, uint32_t&, uint32_t&);
    void detach();
private:
    explicit ScarletLayerTree(Function<void()>&&);
    void assertIsOwner() const;
    void render();
    bool commit();
    void waitUntilPaintingComplete();
    Vector<Ref<CoordinatedPlatformLayer>> layersSnapshot() const;
    Ref<GraphicsLayer> createGraphicsLayer(GraphicsLayer::Type, GraphicsLayerClient&) override;
    bool compositesOnMainThread() const override { return false; }
    void attachLayer(CoordinatedPlatformLayer&) override;
    void detachLayer(CoordinatedPlatformLayer&) override;
    SkiaPaintingEngine& paintingEngine() const override { return *m_paintingEngine; }
    Ref<CoordinatedImageBackingStore> imageBackingStore(Ref<NativeImage>&&) override;
    void notifyCompositionRequired() override;
    bool isCompositionRequiredOrOngoing() const override { return needsComposition(); }
    void requestComposition(CompositionReason) override;
    RunLoop* compositingRunLoop() const override { return &m_workQueue->runLoop(); }
    int maxTextureSize() const override { return m_maxTextureSize; }
    void willPaintTile() override;
    void didPaintTile() override;
    void didPaintTile(const IntRect&) override;

    Function<void()> m_scheduleRenderingUpdate;
    RunLoop& m_ownerRunLoop;
    Ref<WorkQueue> m_workQueue;
    RunLoop::Timer m_timer;
    std::unique_ptr<SkiaPaintingEngine> m_paintingEngine;
    std::shared_ptr<Output> m_output { std::make_shared<Output>() };
    struct Scene;
    MonotonicTime m_nextFrame;
    std::unique_ptr<Scene> m_scene; // Only the compositor accesses its contents.
    HashSet<Ref<CoordinatedPlatformLayer>> m_layers; // Owner only.
    HashMap<uint64_t, Ref<CoordinatedImageBackingStore>> m_imageBackingStores; // Owner only.
    RefPtr<GraphicsLayer> m_rootLayer, m_overlayLayer;
    mutable std::mutex m_commitMutex;
    Vector<Ref<CoordinatedPlatformLayer>> m_pendingLayers, m_removedLayers;
    Vector<Ref<CoordinatedPlatformLayer>> m_pendingRoots;
    bool m_commitPending { false };
    IntSize m_cssViewport, m_deviceViewport;
    float m_deviceScale { 1 };
    int m_maxTextureSize { 0 };
    uint64_t m_epoch { 0 };
    mutable Lock m_paintingLock;
    Condition m_paintingCondition;
    TilePaintStatistics m_tilePaintStatistics WTF_GUARDED_BY_LOCK(m_paintingLock);
    std::atomic<unsigned> m_pendingTiles { 0 };
    std::atomic<bool> m_compositionRequired { true }, m_detached { false }, m_ownerCommitRequired { true }, m_wakeQueued { false };
};
}
#endif
