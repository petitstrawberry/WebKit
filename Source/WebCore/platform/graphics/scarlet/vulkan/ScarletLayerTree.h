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
#include "SkiaCompositingLayer.h"
#include <cstdint>
#include <atomic>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <memory>
#include <wtf/Function.h>
#include <wtf/HashSet.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
namespace WebCore {
class LocalFrameView;
class SkiaPaintingEngine;

// Owns the native scene on the WebCore owner run loop. Layer backing is private
// Ganesh storage; only the embedding's final page images are shared with the UI.
class ScarletLayerTree final : public GraphicsLayerFactory, public CoordinatedPlatformLayer::Client {
public:
    static std::unique_ptr<ScarletLayerTree> create(Function<void()>&& scheduleRenderingUpdate);
    ~ScarletLayerTree();
    GraphicsLayerFactory* factory() { return this; }
    void setRootLayer(GraphicsLayer*);
    void setOverlayLayer(GraphicsLayer*);
    void setViewport(const IntSize& cssViewport, float deviceScale);
    // Commit platform state after Page::finalizeRenderingUpdate performed the layer flush.
    bool flush(LocalFrameView&);
    // Damage is in device pixels. A missing prior damage means an unknown target;
    // an empty prior damage means valid target pixels with nothing owed.
    // Returns whether an animation remains active, not a submission status.
    bool paint(SkCanvas&, std::optional<Damage>& frameDamage,
        const std::optional<Damage>& priorTargetDamage = std::nullopt,
        std::optional<SkColor> clearColor = std::nullopt);
    struct TilePaintStatistics { uint64_t paints { 0 }; uint64_t pixels { 0 }; };
    TilePaintStatistics tilePaintStatistics() const;
    bool needsComposition() const;
    void didSubmit(MonotonicTime);
    void discardPendingPresentationCallbacks();
    void detach();

private:
    explicit ScarletLayerTree(Function<void()>&&);
    void assertIsOwner() const;
    void updateSceneRoot();
    void waitUntilPaintingComplete();
    Vector<Ref<CoordinatedPlatformLayer>> layersSnapshot() const;

    Ref<GraphicsLayer> createGraphicsLayer(GraphicsLayer::Type, GraphicsLayerClient&) override;
    bool compositesOnMainThread() const override { return true; }
    void attachLayer(CoordinatedPlatformLayer&) override;
    void detachLayer(CoordinatedPlatformLayer&) override;
    SkiaPaintingEngine& paintingEngine() const override { return *m_paintingEngine; }
    Ref<CoordinatedImageBackingStore> imageBackingStore(Ref<NativeImage>&&) override;
    void notifyCompositionRequired() override;
    bool isCompositionRequiredOrOngoing() const override { return needsComposition(); }
    void requestComposition(CompositionReason) override;
    RunLoop* compositingRunLoop() const override { return &m_ownerRunLoop; }
    int maxTextureSize() const override;
    void willPaintTile() override;
    void didPaintTile() override;
    void didPaintTile(const IntRect&) override;

    Function<void()> m_scheduleRenderingUpdate;
    RunLoop& m_ownerRunLoop;
    std::unique_ptr<SkiaPaintingEngine> m_paintingEngine;
    const Ref<SkiaCompositingLayer> m_sceneRoot;
    HashSet<Ref<CoordinatedPlatformLayer>> m_layers;
    RefPtr<GraphicsLayer> m_rootLayer;
    RefPtr<GraphicsLayer> m_overlayLayer;
    Vector<Function<void(MonotonicTime)>> m_presentationCallbacks;
    IntSize m_cssViewport;
    float m_deviceScale { 1 };
    mutable Lock m_paintingLock;
    Condition m_paintingCondition;
    TilePaintStatistics m_tilePaintStatistics WTF_GUARDED_BY_LOCK(m_paintingLock);
    std::atomic<unsigned> m_pendingTiles { 0 };
    bool m_compositionRequired { true };
    bool m_runningAnimations { false };
    bool m_detached { false };
};
} // namespace WebCore
#endif
