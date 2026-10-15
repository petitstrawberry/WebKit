/*
 * Copyright (C) 2026 Igalia S.L.
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
#include "SkiaCompositingLayer.h"

#if USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
#include "CoordinatedAnimatedBackingStoreClient.h"
#include "CoordinatedImageBackingStore.h"
#include "CoordinatedPlatformLayerBuffer.h"
#include "CoordinatedPlatformLayerBufferHolePunch.h"
#include "CoordinatedTileBuffer.h"
#include "FilterOperations.h"
#include "FontCache.h"
#include "PlatformDisplay.h"
#include "Region.h"
#include "SkiaBackingStore.h"
#include "SkiaCompositingLayerFilters.h"
#include "SkiaCompositingLayerOverlapRegions.h"
#include "SkiaDamageRegion.h"
#include "SkiaUtilities.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkFont.h>
#include <skia/core/SkPathBuilder.h>
#include <skia/core/SkRRect.h>
#include <skia/effects/SkImageFilters.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/SkImageGanesh.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/utils/SkNoDrawCanvas.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>
#if PLATFORM(SCARLET)
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SkiaCompositingLayer);

static constexpr float s_opacityVisibilityThreshold = 0.01;

#if PLATFORM(SCARLET)
// Owner-thread diagnostics only. No clocks, counters, or output when disabled.
struct ScarletScenePaintProfile {
    bool enabled { [] {
        const char* value = std::getenv("SCARLET_WEBVIEW_PROFILE");
        return value && !std::strcmp(value, "1");
    }() };
    MonotonicTime periodStart;
    uint64_t paints { 0 };
    std::array<uint64_t, 4> phaseMicros { };
    uint64_t intermediateSurfaces { 0 }, maskSurfaces { 0 }, filterSurfaces { 0 };

    static uint64_t micros(MonotonicTime begin, MonotonicTime end)
    {
        double value = (end - begin).microseconds();
        if (!(value > 0))
            return 0;
        return value >= static_cast<double>(UINT64_MAX) ? UINT64_MAX : static_cast<uint64_t>(value);
    }
    static void add(uint64_t& counter, uint64_t value)
    {
        counter = value > UINT64_MAX - counter ? UINT64_MAX : counter + value;
    }
    void record(const void* root, const std::array<MonotonicTime, 5>& times)
    {
        if (!periodStart)
            periodStart = times.front();
        add(paints, 1);
        for (size_t i = 0; i < phaseMicros.size(); ++i)
            add(phaseMicros[i], micros(times[i], times[i + 1]));
        if ((times.back() - periodStart).seconds() < 1)
            return;
        auto printable = [](uint64_t value) { return static_cast<unsigned long long>(value); };
        std::fprintf(stderr, "SCARLET_SCENE_PROFILE root=%p period_us=%llu paints=%llu transforms_us=%llu damage_us=%llu draw_us=%llu cleanup_us=%llu intermediate_surfaces=%llu mask_surfaces=%llu filter_surfaces=%llu\n",
            root, printable(micros(periodStart, times.back())), printable(paints),
            printable(phaseMicros[0]), printable(phaseMicros[1]), printable(phaseMicros[2]), printable(phaseMicros[3]),
            printable(intermediateSurfaces), printable(maskSurfaces), printable(filterSurfaces));
        periodStart = times.back();
        paints = intermediateSurfaces = maskSurfaces = filterSurfaces = 0;
        phaseMicros = { };
    }
};
static ScarletScenePaintProfile& scarletScenePaintProfile()
{
    static thread_local ScarletScenePaintProfile profile;
    return profile;
}
#endif

Ref<SkiaCompositingLayer> SkiaCompositingLayer::create()
{
    return adoptRef(*new SkiaCompositingLayer());
}

SkiaCompositingLayer::~SkiaCompositingLayer() = default;

void SkiaCompositingLayer::invalidate()
{
    m_backingStore = nullptr;
    m_animatedBackingStoreClient = nullptr;
    m_maskImage = nullptr;
    m_imageBackingStore = nullptr;
    m_contentsBuffer = nullptr;

    m_mask = nullptr;
    m_replica = nullptr;
    m_replicatedLayer = nullptr;

    for (auto& child : m_children)
        child->m_parent = nullptr;
    removeFromParent();
}

// The setters below damage once the new state is in place, since damageWholeLayer() reads the layer's
// size to decide what to damage, and what it has to damage is what the layer is about to paint.
void SkiaCompositingLayer::setSize(const FloatSize& size)
{
    if (m_rect.size() == size)
        return;

    m_rect.setSize(size);
    damageWholeLayer();
}

void SkiaCompositingLayer::setOpacity(float opacity)
{
    if (m_opacity == opacity)
        return;

    m_opacity = opacity;
    damageWholeLayer();
    groupPropertyChanged();
}

void SkiaCompositingLayer::setBlendMode(BlendMode blendMode)
{
    auto newBlendMode = blendMode == BlendMode::Normal ? std::nullopt : std::optional(SkiaUtilities::toSkiaBlendMode(blendMode));
    if (m_blendMode == newBlendMode)
        return;

    m_blendMode = newBlendMode;
    groupPropertyChanged();
}

void SkiaCompositingLayer::setChildren(Vector<Ref<SkiaCompositingLayer>>&& newChildren)
{
    if (m_children == newChildren)
        return;

    while (!m_children.isEmpty()) {
        auto child = m_children.takeLast();
        child->m_parent = nullptr;
    }

    m_children = WTF::move(newChildren);
    for (auto& child : m_children) {
        child->removeFromParent();
        child->m_parent = this;
    }
}

void SkiaCompositingLayer::removeFromParent()
{
    RefPtr parent = std::exchange(m_parent, nullptr);
    if (!parent)
        return;

    parent->m_children.removeFirstMatching([this](auto& layer) {
        return layer.ptr() == this;
    });
}

void SkiaCompositingLayer::setUseBackingStore(bool useBackingStore, CoordinatedAnimatedBackingStoreClient* animatedBackingStoreClient)
{
    if (!useBackingStore) {
        m_backingStore = nullptr;
        m_animatedBackingStoreClient = nullptr;
        m_maskImage = nullptr;
        return;
    }

    if (!m_backingStore)
        m_backingStore = makeUnique<SkiaBackingStore>();
    m_animatedBackingStoreClient = animatedBackingStoreClient;
}

void SkiaCompositingLayer::updateBackingStore(CoordinatedBackingStoreProxy::Update&& update, float scale)
{
    if (m_maskImage && !update.isEmpty())
        m_maskImage = nullptr;

    ASSERT(m_backingStore);
    m_backingStore->update(m_rect.size(), scale, WTF::move(update));
}

bool SkiaCompositingLayer::hasPendingBackingStoreTileUpdates() const
{
    return m_backingStore ? m_backingStore->hasPendingTileUpdates() : false;
}

void SkiaCompositingLayer::processPendingTileUpdates()
{
    if (m_backingStore)
        m_backingStore->processPendingTileUpdates();
}

void SkiaCompositingLayer::setImageBackingStore(CoordinatedImageBackingStore* imageBackingStore)
{
    m_imageBackingStore = imageBackingStore;
}

void SkiaCompositingLayer::setContentsBuffer(std::unique_ptr<CoordinatedPlatformLayerBuffer>&& contentsBuffer)
{
    m_contentsBuffer = WTF::move(contentsBuffer);
}

std::unique_ptr<CoordinatedPlatformLayerBuffer> SkiaCompositingLayer::takeContentsBuffer()
{
    return WTF::move(m_contentsBuffer);
}

void SkiaCompositingLayer::setContentsSolidColor(const Color& color)
{
    if (m_contentsSolidColor == color)
        return;

    m_contentsSolidColor = color;
    damageWholeLayer();
}

void SkiaCompositingLayer::setMask(RefPtr<SkiaCompositingLayer>&& mask)
{
    if (m_mask == mask)
        return;

    m_mask = WTF::move(mask);
    groupPropertyChanged();
}

void SkiaCompositingLayer::setReplica(RefPtr<SkiaCompositingLayer>&& replica)
{
    if (m_replica == replica)
        return;

    m_replica = WTF::move(replica);
    if (m_replica)
        m_replica->m_replicatedLayer = this;

    // FIXME: the walk reaches the replica once per replica transform, so the overlap region is computed
    // without it. That covers where a replica appears, but not where a removed one used to be, which
    // needs the replica rect from the previous frame.
    groupPropertyChanged();
}

void SkiaCompositingLayer::setFilters(const FilterOperations& filterOperations)
{
    if (filterOperations.isEmpty()) {
        if (!m_filter)
            return;
        m_filter = std::nullopt;
    } else
        m_filter = { SkiaCompositingLayerFilters::create(filterOperations), filterOperations.outsets() };

    groupPropertyChanged();
}

void SkiaCompositingLayer::setBackdropFilters(const FilterOperations& filterOperations)
{
    m_backdrop.filter = SkiaCompositingLayerFilters::create(filterOperations, SkTileMode::kClamp);
    m_backdrop.filterOperations = filterOperations;
}

void SkiaCompositingLayer::setBackdropFiltersRect(const FloatRoundedRect& clipRect)
{
    m_backdrop.clipRect = clipRect;
}

IntOutsets SkiaCompositingLayer::unclippedFilterOutsets() const
{
    auto filter = this->filter();
    if (!filter || filter->outsets.isZero() || m_masksToBounds || m_mask || m_backdrop.filter)
        return { };

    return filter->outsets;
}

FloatRect SkiaCompositingLayer::paintedLayerRect() const
{
    auto rect = m_rect;
    if (paintsContentsRect())
        rect.unite(m_contentsRect);
    if (m_backdrop.filter)
        rect.unite(m_backdrop.clipRect.rect());

    // A debug border is stroked on the edges of what the layer paints, respect that.
    if (m_debugBorder)
        rect.inflate(m_debugBorder->width / 2);

    // Mirrors what computeOverlapRegions() does with the same outsets.
    rect.expand(toFloatBoxExtent(unclippedFilterOutsets()));

    return rect;
}

Ref<SkiaCompositingLayer> SkiaCompositingLayer::backdropRoot()
{
    if (m_isBackdropRoot)
        return *this;

    if (m_parent)
        return m_parent->backdropRoot();

    if (m_replicatedLayer)
        return m_replicatedLayer->backdropRoot();

    return *this;
}

#if ENABLE(DAMAGE_TRACKING)
void SkiaCompositingLayer::addDamage(Damage&& damage)
{
    // The damage is added not to override the damage that could be inferred from other set* operations.
    if (m_layerDamage)
        m_layerDamage->add(damage);
    else
        m_layerDamage = WTF::move(damage);
}

void SkiaCompositingLayer::LayerRectTracker::recordVisit(uint64_t id, const FloatRect& rectInFrame)
{
    auto& entry = m_entries.add(id, Entry { }).iterator->value;

    // A replicated layer is walked once per replica, so unite the visits rather than let the last one win,
    // or the place the replica moved away from would never be repainted.
    if (!wasVisitedThisFrame(entry)) {
        entry.lastVisitedFrameID = m_currentFrameID;
        entry.currentRect = rectInFrame;
    } else
        entry.currentRect.unite(rectInFrame);
}

void SkiaCompositingLayer::LayerRectTracker::damageStaleLayerRectsAndAdvanceEntries(Damage& frameDamage)
{
    // Runs once the walk has visited every layer, so each entry's currentRect is complete.
    m_entries.removeIf([&](auto& keyAndEntry) {
        auto& entry = keyAndEntry.value;

        // Never visited, so the layer is hidden, out of the tree or already destroyed. Repaint where it
        // was and forget it, and if it ever comes back it gets a fresh entry under the same id.
        if (!wasVisitedThisFrame(entry)) {
            frameDamage.add(entry.previousRect);
            return true;
        }

        // The layer moved, so repaint both where it was and where it now is.
        if (entry.previousRect != entry.currentRect) {
            frameDamage.add(std::exchange(entry.previousRect, entry.currentRect));
            frameDamage.add(entry.currentRect);
        }

        return false;
    });
}

void SkiaCompositingLayer::trackLayerRect(PaintContext& context, const FloatRect& layerRectInFrame)
{
    auto& layerRectTracker = context.collectState->layerRectTracker;

    // Identifiers are never reused, so an entry can outlive its layer without a later layer inheriting it.
    if (!m_layerRectID)
        m_layerRectID = layerRectTracker.generateLayerRectID();

    layerRectTracker.recordVisit(m_layerRectID, layerRectInFrame);
}

void SkiaCompositingLayer::resolveBackdropDamage(const Vector<FloatRect>& backdropRectsInFrame, Damage& frameDamage)
{
    // A backdrop is filtered from what lies under it, so anything changing in it changes the whole
    // backdrop. Walked in paint order, so a damaged backdrop also changes the backdrops above it.
    for (const auto& backdropRect : backdropRectsInFrame) {
        const auto backdropRectToTest = enclosingIntRect(backdropRect);

        // Tested against the damage rects rather than their bounds, or a change anywhere on screen would
        // repaint every backdrop on the page.
        bool damagedUnderneath = false;
        for (const auto& rect : frameDamage) {
            if (rect.intersects(backdropRectToTest)) {
                damagedUnderneath = true;
                break;
            }
        }

        if (damagedUnderneath)
            frameDamage.add(backdropRect);
    }
}
#endif

void SkiaCompositingLayer::setDebugIndicators(Color&& debugBorderColor, std::optional<float> debugBorderWidth, std::optional<unsigned> repaintCount)
{
    std::optional<DebugBorder> debugBorder;
    if (debugBorderColor.isValid())
        debugBorder = { WTF::move(debugBorderColor), debugBorderWidth.value_or(1) };

    if (m_debugBorder == debugBorder && m_repaintCount == repaintCount)
        return;

    m_debugBorder = WTF::move(debugBorder);
    m_repaintCount = repaintCount;
    damageWholeLayer();
}

const TransformationMatrix& SkiaCompositingLayer::localTransform() const
{
    if (!m_animationsState)
        return m_transform;

    return m_animationsState->transform ? m_animationsState->transform.value() : m_transform;
}

const TransformationMatrix& SkiaCompositingLayer::futureLocalTransform() const
{
    if (!m_animationsState)
        return m_transform;

    return m_animationsState->futureTransform ? m_animationsState->futureTransform.value() : localTransform();
}

float SkiaCompositingLayer::opacityForAnimationsState(const AnimationsState* animationsState) const
{
    if (!animationsState)
        return m_opacity;

    return animationsState->opacity.value_or(m_opacity);
}

float SkiaCompositingLayer::opacity() const
{
    return opacityForAnimationsState(m_animationsState ? &*m_animationsState : nullptr);
}

const std::optional<SkiaCompositingLayer::Filter> SkiaCompositingLayer::filter() const
{
    if (!m_animationsState)
        return m_filter;

    return m_animationsState->filter ? m_animationsState->filter : m_filter;
}

bool SkiaCompositingLayer::hasFilter() const
{
    auto filter = this->filter();
    return filter && filter->filter;
}

std::optional<SkiaCompositingLayer::AnimationsState> SkiaCompositingLayer::syncAnimations(MonotonicTime time)
{
    auto damageIfOpacityChanged = [&](const AnimationsState* newState) {
        if (opacity() == opacityForAnimationsState(newState))
            return;

        damageWholeLayer();
        groupPropertyChanged();
        // FIXME: add collectFrameDamageDespiteBeingInvisible?
    };

    // Null once the animation stops, so that falling back to the layer's own filter is a change too.
    auto animatedFilters = [](const AnimationsState* state) -> const FilterOperations* {
        if (!state || !state->isRunning || !state->filterOperations)
            return nullptr;

        return &*state->filterOperations;
    };

    auto damageIfFilterChanged = [&](const AnimationsState* newState) {
        const auto* previous = animatedFilters(m_animationsState ? &*m_animationsState : nullptr);
        const auto* current = animatedFilters(newState);
        if (previous == current || (previous && current && *previous == *current))
            return;

        // Only the layer itself is damaged here. The outsets are handled via paintWithFilterAndMask().
        damageWholeLayer();
    };

    if (m_animations.isEmpty()) {
        damageIfOpacityChanged(nullptr);
        damageIfFilterChanged(nullptr);
        return std::nullopt;
    }

    AcceleratedAnimation::ApplyResult applyResult;
    m_animations.apply(applyResult, time);

    AnimationsState state;
    state.transform = applyResult.transform;
    if (state.transform) {
        // Calculate localTransform 50ms in the future.
        AcceleratedAnimation::ApplyResult futureApplyResult;
        m_animations.apply(futureApplyResult, time + 50_ms);
        state.futureTransform = futureApplyResult.transform;
    }
    state.opacity = applyResult.opacity;
    if (applyResult.filters) {
        state.filter = { SkiaCompositingLayerFilters::create(*applyResult.filters), applyResult.filters->outsets() };
        state.filterOperations = applyResult.filters;
    }
    state.isRunning = applyResult.hasRunningAnimations;

    damageIfOpacityChanged(&state);
    damageIfFilterChanged(&state);
    return state;
}

bool SkiaCompositingLayer::computeTransformsAndAnimations(const TransformationMatrix& parentTransform, const TransformationMatrix& futureParentTransform, MonotonicTime time)
{
    m_animationsState = syncAnimations(time);
    bool hasRunningAnimations = m_animationsState ? m_animationsState->isRunning : false;

    TransformationMatrix combinedForChildren;
    TransformationMatrix futureCombinedForChildren;

    if (!m_rect.isEmpty() || !m_masksToBounds) {
#if ENABLE(DAMAGE_TRACKING)
        TransformationMatrix previousTransform = m_transforms.combined;
#endif

        FloatPoint origin(m_anchorPoint.x(), m_anchorPoint.y());
        origin.scale(m_rect.size().width(), m_rect.size().height());
        m_transforms.combined = parentTransform;
        m_transforms.combined
            .translate3d(origin.x() + (m_position.x() - m_boundsOrigin.x()), origin.y() + (m_position.y() - m_boundsOrigin.y()), m_anchorPoint.z())
            .multiply(localTransform());

        combinedForChildren = m_transforms.combined;
        m_transforms.combined.translate3d(-origin.x(), -origin.y(), -m_anchorPoint.z());

        if (isReplica())
            m_transforms.combined.translate(-m_position.x(), -m_position.y());

        if (!m_preserves3D)
            combinedForChildren.flatten();
        combinedForChildren.multiply(m_childrenTransform);
        combinedForChildren.translate3d(-origin.x(), -origin.y(), -m_anchorPoint.z());

        m_transforms.futureCombined = futureParentTransform;
        m_transforms.futureCombined
            .translate3d(origin.x() + (m_position.x() - m_boundsOrigin.x()), origin.y() + (m_position.y() - m_boundsOrigin.y()), m_anchorPoint.z())
            .multiply(futureLocalTransform());

        futureCombinedForChildren = m_transforms.futureCombined;
        m_transforms.futureCombined.translate3d(-origin.x(), -origin.y(), -m_anchorPoint.z());

        if (isReplica())
            m_transforms.futureCombined.translate(-m_position.x(), -m_position.y());

        if (!m_preserves3D)
            futureCombinedForChildren.flatten();
        futureCombinedForChildren.multiply(m_childrenTransform);
        futureCombinedForChildren.translate3d(-origin.x(), -origin.y(), -m_anchorPoint.z());

#if ENABLE(DAMAGE_TRACKING)
        if (previousTransform != m_transforms.combined)
            damageWholeLayer();
#endif

        m_visible = m_backfaceVisibility || !m_transforms.combined.isBackFaceVisible();

        if (m_animatedBackingStoreClient)
            m_animatedBackingStoreClient->requestBackingStoreUpdateIfNeeded(m_transforms.futureCombined);
    }

    if (m_mask) {
        auto& maskParent = m_replicatedLayer ? *m_replicatedLayer : *this;
        hasRunningAnimations |= m_mask->computeTransformsAndAnimations(maskParent.m_transforms.combined, maskParent.m_transforms.futureCombined, time);
    }
    if (m_replica)
        hasRunningAnimations |= m_replica->computeTransformsAndAnimations(m_transforms.combined, m_transforms.futureCombined, time);

    m_shouldBlend = !!m_blendMode;
    for (auto& child : m_children) {
        hasRunningAnimations |= child->computeTransformsAndAnimations(combinedForChildren, futureCombinedForChildren, time);
        m_shouldBlend |= !!child->m_blendMode;
    }

    // If the layer is invisible because of opacity and there's no opacity animation, the content won't
    // be visible ever, so triggering repaints doesn't make sense.
    if (!m_opacity && !(m_animationsState && m_animationsState->opacity))
        return false;

    return hasRunningAnimations;
}

bool SkiaCompositingLayer::paint(SkCanvas& canvas, std::optional<Damage>& frameDamage, const std::optional<Damage>& priorTargetDamage, std::optional<SkColor> clearColor, Vector<Function<void(MonotonicTime)>>* presentationCallbacks)
{
#if PLATFORM(SCARLET)
    auto& profile = scarletScenePaintProfile();
    auto profileStart = profile.enabled ? MonotonicTime::now() : MonotonicTime { };
#endif
    // Both walks below assume the animations have been applied and the transforms computed.
    bool hasRunningAnimations = computeTransformsAndAnimations({ }, { }, MonotonicTime::now());
#if PLATFORM(SCARLET)
    auto profileTransformsEnd = profile.enabled ? MonotonicTime::now() : MonotonicTime { };
#endif

#if ENABLE(DAMAGE_TRACKING)
    // Collect the damage in a walk of its own first, so it is complete before the walk that draws. Each
    // layer's damage stays until recursiveCleanUpAfterPaint() clears it at the end of the frame.
    if (frameDamage) {
        // Only the layer paint() is called on needs the table, so only the root ever allocates one.
        if (!m_layerRectTracker)
            lazyInitialize(m_layerRectTracker, makeUnique<LayerRectTracker>());
        m_layerRectTracker->advanceToNextFrame();

        PaintContext collectContext;
        collectContext.collectState.emplace(*frameDamage, *m_layerRectTracker);

        // Sized like the canvas that will be drawn into, so both walks clip against the same device
        // bounds. The layer's own size is in layer coordinates, which the root transform scales to the
        // device by the device pixel ratio.
        const auto canvasSize = canvas.getBaseLayerSize();
        SkNoDrawCanvas noDrawCanvas(canvasSize.width(), canvasSize.height());
        recursivePaint(noDrawCanvas, collectContext);

        m_layerRectTracker->damageStaleLayerRectsAndAdvanceEntries(*frameDamage);
        resolveBackdropDamage(collectContext.collectState->backdropRectsInFrame, *frameDamage);
    }
#else
    UNUSED_PARAM(frameDamage);
#endif

    // The region this target must redraw is the damage still on its record from before combined with
    // this frame's. No prior damage means the target cannot be trusted, so repaint the whole of it.
    std::optional<SkiaDamageRegion> damageRegion;
#if ENABLE(DAMAGE_TRACKING)
    if (priorTargetDamage) {
        // The damage is in device space, so the surface size the region is tested against must be too.
        // m_rect is in layer coordinates, which the root transform scales to the device by the device
        // pixel ratio - use the canvas device size instead, as the collect walk above already does.
        const auto deviceSize = canvas.getBaseLayerSize();
        const IntSize surfaceSize(deviceSize.width(), deviceSize.height());
        if (frameDamage) {
            auto repaintRegion = *priorTargetDamage;
            repaintRegion.add(*frameDamage);
            damageRegion = SkiaDamageRegion::create(repaintRegion, surfaceSize);
        } else
            damageRegion = SkiaDamageRegion::create(*priorTargetDamage, surfaceSize);
    }
#else
    UNUSED_PARAM(priorTargetDamage);
#endif

#if PLATFORM(SCARLET)
    auto profileDamageEnd = profile.enabled ? MonotonicTime::now() : MonotonicTime { };
#endif
    // An empty region means the target already holds the frame, so draw nothing and skip the clear.
    const bool skipDraw = damageRegion && damageRegion->isEmpty();
    if (!skipDraw) {
        // Clear what will be redrawn - only the damage rects when compositing from the damage, which keeps
        // undamaged content and composites translucent content once, or the whole target otherwise.
        if (clearColor) {
            if (damageRegion) {
                SkPaint clearPaint;
                clearPaint.setColor(*clearColor);
                clearPaint.setBlendMode(SkBlendMode::kSrc);
                damageRegion->fillCanvasInDeviceSpace(canvas, clearPaint);
            } else
                canvas.clear(*clearColor);
        }

        PaintContext context;
        context.compositingDamageRegion = WTF::move(damageRegion);
        context.presentationCallbacks = presentationCallbacks;

        recursivePaint(canvas, context);
        context.imageSetBatch.flushIfNeeded(canvas);
    }

#if PLATFORM(SCARLET)
    auto profileDrawEnd = profile.enabled ? MonotonicTime::now() : MonotonicTime { };
#endif
    recursiveCleanUpAfterPaint();
#if PLATFORM(SCARLET)
    if (profile.enabled)
        profile.record(this, { profileStart, profileTransformsEnd, profileDamageEnd, profileDrawEnd, MonotonicTime::now() });
#endif

    return hasRunningAnimations;
}

void SkiaCompositingLayer::clipRect(SkCanvas& canvas, const FloatRoundedRect& rect, const TransformationMatrix& transform)
{
    if (transform.isIdentity()) {
        if (rect.hasNonZeroRadii())
            canvas.clipRRect(SkRRect(rect), true);
        else
            canvas.clipRect(SkRect(rect.rect()));
        return;
    }

    auto matrix = SkM44(transform).asM33();
    if (rect.hasNonZeroRadii())
        canvas.clipPath(SkPath::RRect(SkRRect(rect)).makeTransform(matrix), true);
    else if (matrix.rectStaysRect())
        canvas.clipRect(matrix.mapRect(SkRect(rect.rect())));
    else
        canvas.clipPath(SkPath::Rect(SkRect(rect.rect())).makeTransform(matrix));
}

void SkiaCompositingLayer::paintSelf(SkCanvas& canvas, PaintContext& context)
{
#if ENABLE(DAMAGE_TRACKING)
    // Collected before the content check, because a backdrop filter paints without content of its own.
    if (!context.shouldDraw()) {
        collectFrameDamage(canvas, context);
        return;
    }
#endif

    if (!hasVisiblePaintableContent())
        return;

    paintContents(canvas, context);

    // Drawn in the content stream, so content above hides the border and it shares
    // the group's opacity, filter and mask - similar to the Viz design.
    if (m_debugBorder)
        paintDebugBorder(canvas, context);
}

TransformationMatrix SkiaCompositingLayer::combinedTransform(const PaintContext& context) const
{
    TransformationMatrix transform(context.accumulatedReplicaTransform);
    transform.multiply(m_transforms.combined);
    return transform;
}

static std::optional<SkMatrix> rotateContentsIfNeeded(CoordinatedPlatformLayerBuffer::Rotation rotation, const FloatRect& contentsRect)
{
    auto rotate = [](float degrees, float x, float y) {
        auto matrix = SkMatrix::Translate(x, y);
        matrix.preRotate(degrees);
        return matrix;
    };

    switch (rotation) {
    case CoordinatedPlatformLayerBuffer::Rotation::None:
        return std::nullopt;
    case CoordinatedPlatformLayerBuffer::Rotation::Right:
        return rotate(90, contentsRect.maxX(), contentsRect.y());
    case CoordinatedPlatformLayerBuffer::Rotation::UpsideDown:
        return rotate(180, contentsRect.maxX(), contentsRect.maxY());
    case CoordinatedPlatformLayerBuffer::Rotation::Left:
        return rotate(270, contentsRect.x(), contentsRect.maxY());
    }
    return std::nullopt;
}

void SkiaCompositingLayer::paintContents(SkCanvas& canvas, PaintContext& context)
{
    // Important: the walk does not clip the canvas to the damage, so every content draw below limits
    // itself, with drawRectRestricted() or drawImageRectRestricted() when it draws now, or by passing the
    // region to the batch when it draws later. A draw that does not paints over undamaged pixels and
    // composites translucent content twice, so any content type added here has to limit itself too. A set
    // region is never empty, because paint() turns an empty one into a no-op.
    ASSERT(!context.compositingDamageRegion || !context.compositingDamageRegion->isEmpty());

    SkM44 transform(combinedTransform(context));

    const auto ctm = transform.asM33();
    const auto contentsInDevice = SkMatrix::Concat(canvas.getLocalToDeviceAs3x3(), ctm).mapRect(SkRect(m_contentsRect));
    bool enableAntialias = !ctm.preservesAxisAlignment() && !ctm.preservesRightAngles();

    auto shouldBlend = [&]() -> bool {
        if (m_contentsOpaque)
            return false;

        if (m_backingStore)
            return true;

        if (m_contentsBuffer)
            return !m_contentsBuffer->isOpaque();

        if (m_imageBackingStore) {
            if (const auto* buffer = m_imageBackingStore->buffer())
                return !buffer->isOpaque();
        }

        return true;
    };

    // When the layer contents are fully opaque and composited at full opacity with the default
    // (source-over) blend mode, drawing the batched tiles/images with source-over needlessly keeps
    // GL_BLEND enabled for every draw. Compositing with source (kSrc) instead lets Skia's blend
    // formula collapse to (kOne, kZero) so GL_BLEND is disabled -- mirroring TextureMapper's per-draw
    // ShouldBlend decision and saving substantial bandwidth on tiled GPUs (e.g. Vivante/etnaviv).
    // FIXME: a color filter forces source-over conservatively -- it may turn opaque contents
    // translucent, and kSrc would then write those pixels without blending. We could inspect the
    // filter and still use kSrc when it provably keeps the contents opaque.
    bool forcedSrcBlendMode = false;
    auto batchBlendMode = context.blendMode;
    if (!batchBlendMode && !context.colorFilter && !shouldBlend() && context.opacity == 1) {
        batchBlendMode = SkBlendMode::kSrc;
        forcedSrcBlendMode = true;
    }
    context.imageSetBatch.updatePaintProperties(canvas, context.colorFilter, batchBlendMode);

    auto setupPaint = [&] -> SkPaint {
        SkPaint paint;
        paint.setStyle(SkPaint::kFill_Style);
        paint.setAntiAlias(enableAntialias);
        paint.setAlphaf(context.opacity);
        if (context.blendMode)
            paint.setBlendMode(*context.blendMode);
        if (context.colorFilter)
            paint.setColorFilter(context.colorFilter);
        return paint;
    };

    if (m_backingStore)
        context.imageSetBatch.addImageSet(canvas, *m_backingStore, transform, context.opacity, enableAntialias, context.damageRegionOrNull(), setupPaint());
    else if (m_backgroundColor.isValid() && m_backgroundColor.isVisible()) {
        ScopedFlush autoFlush(canvas, context.imageSetBatch, ScopedFlush::Mode::FlushBefore);
        canvas.concat(transform);
        SkPaint paint = setupPaint();
        paint.setColor(SkColor(m_backgroundColor.colorWithAlphaMultipliedBy(context.opacity)));
        drawRectRestricted(canvas, context.damageRegionOrNull(), SkRect(m_rect), paint);
    }

    if (m_contentsSolidColor.isValid() && m_contentsSolidColor.isVisible()) {
        ScopedFlush autoFlush(canvas, context.imageSetBatch, ScopedFlush::Mode::FlushBefore);
        canvas.concat(transform);
        SkPaint paint = setupPaint();
        paint.setColor(SkColor(m_contentsSolidColor.colorWithAlphaMultipliedBy(context.opacity)));
        drawRectRestricted(canvas, context.damageRegionOrNull(), SkRect(m_contentsRect), paint);
    } else if (m_contentsBuffer || m_imageBackingStore) {
        bool shouldPaintNow = [&] {
            if (m_contentsClipPath)
                return true;

            if (m_contentsClippingRect.hasNonZeroRadii())
                return true;

            if (!m_contentsBuffer && !m_contentsTiling.size.isEmpty())
                return true;

#if ENABLE(VIDEO)
            if (is<CoordinatedPlatformLayerBufferHolePunch>(m_contentsBuffer))
                return true;
#endif

            // FIXME: clip is not correctly applied with batched painting.
            if (!m_contentsClippingRect.rect().contains(m_contentsRect))
                return true;

            return false;
        }();

        ScopedFlush autoFlush(canvas, context.imageSetBatch, shouldPaintNow ? ScopedFlush::Mode::FlushBefore : ScopedFlush::Mode::DoNothing);
        if (shouldPaintNow) {
            canvas.concat(transform);

            // A corner shape the clipping rect cannot express arrives as a path instead; the rect has had
            // its radii dropped in that case, so the path is the whole clip.
            if (m_contentsClipPath)
                canvas.clipPath(*m_contentsClipPath, true);
            else if (m_contentsClippingRect.hasNonZeroRadii() || !m_contentsClippingRect.rect().contains(m_contentsRect))
                clipRect(canvas, m_contentsClippingRect);
        }

        sk_sp<SkImage> image;
        std::optional<SkMatrix> rotationMatrix;
        auto imageRect = m_contentsRect;

        if (m_contentsBuffer) {
#if ENABLE(VIDEO)
            if (is<CoordinatedPlatformLayerBufferHolePunch>(*m_contentsBuffer)) {
#if USE(GSTREAMER)
                TransformationMatrix matrix = canvas.getLocalToDevice();
                downcast<CoordinatedPlatformLayerBufferHolePunch>(*m_contentsBuffer).setHolePunchVideoRectangle(enclosingIntRect(matrix.mapRect(m_contentsRect)));
#endif
                SkPaint paint = setupPaint();
                paint.setBlendMode(SkBlendMode::kClear);
                drawRectRestricted(canvas, context.damageRegionOrNull(), SkRect(m_contentsRect), paint);
            } else
#endif // ENABLE(VIDEO)
                image = m_contentsBuffer->skiaImage();

            auto rotation = m_contentsBuffer->rotation();
            rotationMatrix = rotateContentsIfNeeded(rotation, m_contentsRect);
            if (rotationMatrix) {
                imageRect.setLocation({ });
                if (rotation == CoordinatedPlatformLayerBuffer::Rotation::Right || rotation == CoordinatedPlatformLayerBuffer::Rotation::Left)
                    imageRect.setSize(m_contentsRect.size().transposedSize());
            }
        } else if (auto* buffer = m_imageBackingStore->buffer()) {
            image = buffer->skiaImage();
            if (!m_contentsTiling.size.isEmpty()) {
                sk_sp<SkImage> tileImage = std::exchange(image, nullptr);
                SkMatrix matrix;
                matrix.setScale(m_contentsTiling.size.width() / tileImage->width(), m_contentsTiling.size.height() / tileImage->height());
                matrix.postTranslate(m_contentsRect.x() - m_contentsTiling.phase.width(), m_contentsRect.y() - m_contentsTiling.phase.height());
                SkPaint paint = setupPaint();
                // The shader matrix maps the tile image into layer space, so it needs to be taken into account here.
                const auto sampling = SkiaUtilities::samplingOptionsForMatrix(SkMatrix::Concat(canvas.getLocalToDeviceAs3x3(), matrix));
                // Clamp tiles that do not repeat, so bilinear sampling at an edge cannot pick up the opposite one.
                auto tileMode = [](float tileExtent, float contentsExtent, float phase) {
                    return (phase || tileExtent < contentsExtent) ? SkTileMode::kRepeat : SkTileMode::kClamp;
                };
                paint.setShader(tileImage->makeShader(
                    tileMode(m_contentsTiling.size.width(), m_contentsRect.width(), m_contentsTiling.phase.width()),
                    tileMode(m_contentsTiling.size.height(), m_contentsRect.height(), m_contentsTiling.phase.height()),
                    sampling, matrix));
                drawRectRestricted(canvas, context.damageRegionOrNull(), SkRect(m_contentsRect), paint);
            }
        }

        if (image) {
            if (m_contentsBuffer && context.presentationCallbacks && context.opacity > 0
                && contentsInDevice.intersects(SkRect::Make(canvas.getDeviceClipBounds()))
                && (!context.damageRegionOrNull() || context.damageRegionOrNull()->intersects(contentsInDevice))) {
                if (auto callback = m_contentsBuffer->takePresentationCallback())
                    context.presentationCallbacks->append(WTF::move(callback));
            }
            if (shouldPaintNow) {
                if (rotationMatrix)
                    canvas.concat(*rotationMatrix);
                SkPaint paint = setupPaint();
                const SkRect srcRect = SkRect::MakeSize(SkSize::Make(image->dimensions()));
                const SkRect dstRect = SkRect(imageRect);
                const auto sampling = SkiaUtilities::samplingOptionsForImageDraw(canvas.getLocalToDeviceAs3x3(), srcRect, dstRect);
                drawImageRectRestricted(canvas, context.damageRegionOrNull(), image.get(), srcRect, dstRect, sampling, &paint);
            } else {
                if (rotationMatrix)
                    transform.preConcat(*rotationMatrix);

                // The contents image composites over the backing store, so it must use SrcOver always.
                if (forcedSrcBlendMode && m_backingStore)
                    context.imageSetBatch.updatePaintProperties(canvas, context.colorFilter, context.blendMode);

                context.imageSetBatch.addImage(canvas, image, imageRect, transform, context.opacity, enableAntialias, context.damageRegionOrNull(), setupPaint());
            }
        }
    }
}

#if ENABLE(DAMAGE_TRACKING)
void SkiaCompositingLayer::collectFrameDamage(SkCanvas& canvas, PaintContext& context)
{
    if (!damagePropagationEnabled() || !contributesToFrame())
        return;

    const auto transform = combinedTransform(context);
    const IntRect clipBounds = canvas.getDeviceClipBounds();
    auto layerRectInFrame = FloatRect(projectedBoundingBox(transform, paintedLayerRect(), clipBounds));

    trackLayerRect(context, layerRectInFrame);

    if (!m_layerDamage)
        return;

    // A Damage is sized to the layer, so it cannot express a contents rect that overhangs it. Whole-layer
    // damage, which is what a layer-state change produces, therefore damages what the layer paints.
    if (m_layerDamage->mode() == Damage::Mode::Full) {
        context.collectState->frameDamage.add(layerRectInFrame);
        return;
    }

    for (const auto& rect : *m_layerDamage)
        context.collectState->frameDamage.add(projectedBoundingBox(transform, FloatRect(rect), clipBounds));
}

void SkiaCompositingLayer::collectBackdropDamage(SkCanvas& canvas, PaintContext& context)
{
    if (!damagePropagationEnabled())
        return;

    // The filter samples the whole backdrop, so anything changing underneath changes every pixel of it. Only
    // the rect is collected here, because what changed underneath is only known once the walk has finished.
    // The backdrop root's subtree is walked by the walk itself, so it is not walked again.
    const auto transform = combinedTransform(context);
    context.collectState->backdropRectsInFrame.append(FloatRect(projectedBoundingBox(transform, m_backdrop.clipRect.rect(), canvas.getDeviceClipBounds())));
}

bool SkiaCompositingLayer::hasGroupPropertyDamage() const
{
    // The walk never reaches a mask, so a changed mask counts as a change to the group it masks.
    return m_groupPropertyChanged || (m_mask && m_mask->hasLayerDamage());
}

bool SkiaCompositingLayer::hasDamageInSubtree() const
{
    if (hasLayerDamage() || hasGroupPropertyDamage())
        return true;

    if (m_replica && m_replica->hasDamageInSubtree())
        return true;

    for (const auto& child : m_children) {
        if (child->hasDamageInSubtree())
            return true;
    }

    return false;
}

void SkiaCompositingLayer::addGroupDamage(SkCanvas& canvas, PaintContext& context, const Vector<IntRect, 1>& overlapRects)
{
    if (!damagePropagationEnabled())
        return;

    const IntRect clipBounds = canvas.getDeviceClipBounds();

    auto layerRectInFrame = projectedBoundingBox(combinedTransform(context), paintedLayerRect(), clipBounds);
    if (!layerRectInFrame.isEmpty())
        context.collectState->frameDamage.add(layerRectInFrame);

    for (const auto& rect : overlapRects) {
        auto damageRect = rect;
        damageRect.intersect(clipBounds);
        if (!damageRect.isEmpty())
            context.collectState->frameDamage.add(damageRect);
    }
}

void SkiaCompositingLayer::collectGroupDamage(SkCanvas& canvas, PaintContext& context)
{
    if (!damagePropagationEnabled())
        return;

    // The walk never reaches a mask, so its damage is collected here, and a mask that only moves is
    // damaged by computing the transforms, which does reach it. A changed mask changes what the masked
    // layer composites to, so that is damaged rather than the mask itself.
    if (!hasGroupPropertyDamage())
        return;

    addGroupDamage(canvas, context, computeConsolidatedOverlapRegionRects(canvas, context, ComputeOverlapRegionMode::Union));
}

#endif

void SkiaCompositingLayer::paintDebugBorder(SkCanvas& canvas, PaintContext& context)
{
    ASSERT(m_debugBorder);

    ScopedFlush autoFlush(canvas, context.imageSetBatch, ScopedFlush::Mode::FlushBefore);
    canvas.concat(SkM44(combinedTransform(context)));

    SkPaint borderPaint;
    borderPaint.setStyle(SkPaint::kStroke_Style);
    borderPaint.setColor(SkColor(m_debugBorder->color));
    borderPaint.setStrokeWidth(m_debugBorder->width);
    borderPaint.setAntiAlias(true);

    if (m_backingStore)
        m_backingStore->drawDebugBorders(canvas, borderPaint);
    if (paintsContentsRect())
        canvas.drawRect(SkRect(m_contentsRect), borderPaint);
}

void SkiaCompositingLayer::paintRepaintCounter(SkCanvas& canvas, PaintContext& context)
{
    ASSERT(m_repaintCount);

    if (!hasVisiblePaintableContent())
        return;

    // The counter is drawn in device space, so compose the layer transform here rather than concatenating
    // it onto the canvas only to reset the matrix again below.
    SkPoint deviceOrigin { 0, 0 };
    const auto mapped = (canvas.getLocalToDevice() * SkM44(combinedTransform(context))).map(0, 0, 0, 1);
    if (std::abs(mapped.w) > std::numeric_limits<float>::epsilon())
        deviceOrigin = { mapped.x / mapped.w, mapped.y / mapped.w };
    else
        deviceOrigin = { mapped.x, mapped.y };

    constexpr float pointSize = 14;
    constexpr float padding = 3;

    static SkFont font = [] {
        auto typeface = FontCache::forCurrentThread().fontManager().matchFamilyStyle("monospace", SkFontStyle::Bold());
        SkFont f(typeface, pointSize);
        f.setEdging(SkFont::Edging::kAntiAlias);
        f.setSubpixel(true);
        return f;
    }();

    if (m_repaintCountOverlay.count != m_repaintCount) {
        m_repaintCountOverlay.count = m_repaintCount;
        m_repaintCountOverlay.string = String::number(*m_repaintCount).ascii();
        SkRect textBounds;
        font.measureText(m_repaintCountOverlay.string.data(), m_repaintCountOverlay.string.length(), SkTextEncoding::kUTF8, &textBounds);
        m_repaintCountOverlay.backgroundWidth = textBounds.width() + padding * 2;
        m_repaintCountOverlay.backgroundHeight = textBounds.height() + padding * 2;
        m_repaintCountOverlay.baselineOffset = -textBounds.fTop + padding;
    }

    ScopedFlush autoFlush(canvas, context.imageSetBatch, ScopedFlush::Mode::FlushBefore);
    canvas.resetMatrix();

    SkPaint backgroundPaint;
    backgroundPaint.setColor(m_debugBorder ? SkColor(m_debugBorder->color) : SK_ColorBLACK);
    backgroundPaint.setStyle(SkPaint::kFill_Style);
    canvas.drawRect(SkRect::MakeXYWH(deviceOrigin.x(), deviceOrigin.y(), m_repaintCountOverlay.backgroundWidth, m_repaintCountOverlay.backgroundHeight), backgroundPaint);

    SkPaint textPaint;
    textPaint.setColor(SK_ColorWHITE);
    textPaint.setAntiAlias(true);
    canvas.drawString(m_repaintCountOverlay.string.data(), deviceOrigin.x() + padding, deviceOrigin.y() + m_repaintCountOverlay.baselineOffset, font, textPaint);
}

bool SkiaCompositingLayer::stopPaintingIntoBackdropIfNeeded(PaintContext& context)
{
    if (!m_backdrop.filter || context.paintingBackdropForLayer != this)
        return false;

    context.skipAfterBackdrop = true;
    return true;
}

void SkiaCompositingLayer::paintSelfAndChildren(SkCanvas& canvas, PaintContext& context)
{
    if (stopPaintingIntoBackdropIfNeeded(context))
        return;

    paintSelf(canvas, context);

    if (m_children.isEmpty())
        return;

    auto canSkipClip = [&](const FloatRoundedRect& rect, const TransformationMatrix& transform) {
        if (rect.hasNonZeroRadii())
            return false;

        // We can only skip clipping for layers having one child that is a leaf.
        if (m_children.size() != 1 || !m_children[0]->m_children.isEmpty())
            return false;

        // We don't need to clip if the child is not visible.
        if (!m_children[0]->isVisible())
            return true;

        // If the child has a replica, the local bounds don't include the replicated content.
        if (m_children[0]->m_replica)
            return false;

        // Do not skip the clip if the child has a backdrop filter.
        if (m_children[0]->m_backdrop.filter)
            return false;

        auto matrix = canvas.getLocalToDeviceAs3x3() * SkM44(transform).asM33();
        if (!matrix.rectStaysRect())
            return false;

        // We don't need to clip if the clipped area is bigger or equal than the child bounds.
        auto childMatrix = canvas.getLocalToDeviceAs3x3() * SkM44(m_children[0]->m_transforms.combined).asM33();
        FloatRect childBounds;
        if (m_children[0]->m_backingStore)
            childBounds = m_children[0]->m_rect;
        if (m_children[0]->m_contentsBuffer || m_children[0]->m_imageBackingStore || (m_children[0]->m_contentsSolidColor.isValid() && m_children[0]->m_contentsSolidColor.isVisible()))
            childBounds.unite(m_children[0]->m_contentsRect);

        if (auto childFilter = m_children[0]->filter(); childFilter && !childFilter->outsets.isZero() && !m_children[0]->m_masksToBounds && !m_children[0]->m_mask)
            childBounds.expand(toFloatBoxExtent(childFilter->outsets));

        return matrix.mapRect(SkRect(rect.rect())).contains(childMatrix.mapRect(SkRect(childBounds)));
    };

    const bool contentsRectClipsDescendants = !m_preserves3D && m_contentsRectClipsDescendants && (m_contentsClipPath || m_contentsClippingRect.hasNonZeroRadii() || !m_contentsClippingRect.rect().contains(m_contentsRect));
    const bool masksToBounds = !m_preserves3D && m_masksToBounds;
    TransformationMatrix clipTransform;
    FloatRoundedRect clippingRect;
    if (masksToBounds || contentsRectClipsDescendants) {
        clipTransform = context.accumulatedReplicaTransform;
        clipTransform.multiply(m_transforms.combined);
        if (!contentsRectClipsDescendants)
            clipTransform.translate(m_boundsOrigin.x(), m_boundsOrigin.y());

        FloatRoundedRect rect = contentsRectClipsDescendants ? m_contentsClippingRect : FloatRoundedRect(m_rect);
        if (!canSkipClip(rect, clipTransform))
            clippingRect = rect;
    }

    ScopedFlush autoFlush(canvas, context.imageSetBatch, clippingRect.isEmpty() ? ScopedFlush::Mode::DoNothing : ScopedFlush::Mode::FlushBeforeAndAfter);
    if (!clippingRect.isEmpty())
        clipRect(canvas, clippingRect, clipTransform);

    for (auto& child : m_children)
        child->recursivePaint(canvas, context);
}

bool SkiaCompositingLayer::isVisible() const
{
    if (m_rect.isEmpty() && (m_masksToBounds || m_children.isEmpty()))
        return false;
    if (!m_visible && m_children.isEmpty())
        return false;
    if (!m_contentsVisible && m_children.isEmpty())
        return false;
    if (!hasVisualContent() && !m_backdrop.filter && m_children.isEmpty())
        return false;
    if (opacity() < s_opacityVisibilityThreshold)
        return false;
    return true;
}

TransformationMatrix SkiaCompositingLayer::replicaTransform() const
{
    return TransformationMatrix(m_replica->m_transforms.combined)
        .multiply(m_transforms.combined.inverse().value_or(TransformationMatrix()));
}

static void clipRectIgnoringCanvasMatrix(SkCanvas& canvas, const IntRect& rect)
{
    const auto matrix = canvas.getLocalToDevice();
    canvas.setMatrix(SkM44());
    canvas.clipIRect(rect);
    canvas.setMatrix(matrix);
}

// Scarlet records the entire scene on one owner-thread GrDirectContext and
// submits it once from ScarletSkiaVulkan::submitAsync. Snapshot images retain
// their proxies, and Ganesh render-task dependencies order intermediate writes
// before their consumers without an early flush or submission.
sk_sp<SkImage> SkiaCompositingLayer::maskImage()
{
    if (m_maskImage)
        return m_maskImage;

    if (!m_backingStore)
        return nullptr;

    // Paint the mask at the same scale the tiles were painted.
    auto scale = m_backingStore->scale();
    auto rect = m_rect;
    rect.scale(scale);

    auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
    auto imageInfo = SkImageInfo::Make(rect.width(), rect.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
#if PLATFORM(SCARLET)
    if (auto& profile = scarletScenePaintProfile(); profile.enabled)
        ScarletScenePaintProfile::add(profile.maskSurfaces, 1);
#endif
    auto surface = SkSurfaces::RenderTarget(grContext, skgpu::Budgeted::kNo, imageInfo, 0, kTopLeft_GrSurfaceOrigin, nullptr);
    if (!surface)
        return nullptr;

    auto* surfaceCanvas = surface->getCanvas();
    if (!surfaceCanvas)
        return nullptr;

    surfaceCanvas->clear(SK_ColorTRANSPARENT);
    SkPaint paint;
    surfaceCanvas->scale(scale, scale);
    m_backingStore->paintToCanvas(*surfaceCanvas, paint);
#if !PLATFORM(SCARLET)
    grContext->flushAndSubmit(surface.get(), GrSyncCpu::kNo);
#endif
    m_maskImage = surface->makeImageSnapshot();
    return m_maskImage;
}

void SkiaCompositingLayer::paintWithIntermediateSurface(SkCanvas& canvas, PaintContext& context, const IntRect& contentsRect, SkPaint* paint, NOESCAPE const PaintFunction& paintFunction)
{
    auto bounds = canvas.getDeviceClipBounds();
    if (bounds.isEmpty())
        return;

    auto surfaceRect = intersection(bounds, contentsRect);
    if (surfaceRect.isEmpty())
        return;

#if ENABLE(DAMAGE_TRACKING)
    if (!context.shouldDraw()) {
        paintFunction(canvas, context);
        return;
    }
#endif

    auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
    auto imageInfo = SkImageInfo::Make(surfaceRect.width(), surfaceRect.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
#if PLATFORM(SCARLET)
    if (auto& profile = scarletScenePaintProfile(); profile.enabled)
        ScarletScenePaintProfile::add(profile.intermediateSurfaces, 1);
#endif
    auto surface = SkSurfaces::RenderTarget(grContext, skgpu::Budgeted::kNo, imageInfo, 0, kTopLeft_GrSurfaceOrigin, nullptr);
    if (!surface)
        return;

    auto* surfaceCanvas = surface->getCanvas();
    if (!surfaceCanvas)
        return;

    context.imageSetBatch.flushIfNeeded(canvas);

    surfaceCanvas->clear(SK_ColorTRANSPARENT);
    surfaceCanvas->translate(-static_cast<float>(surfaceRect.x()), -static_cast<float>(surfaceRect.y()));
    surfaceCanvas->concat(canvas.getLocalToDevice());

    {
        // The filter may sample outside the damage, so paint the whole subtree and limit only the composite.
        SetForScope scopedNoDamageRestriction(context.compositingDamageRegion, std::nullopt);
        paintFunction(*surfaceCanvas, context);
        context.imageSetBatch.flushIfNeeded(*surfaceCanvas);
    }
#if !PLATFORM(SCARLET)
    grContext->flushAndSubmit(surface.get(), GrSyncCpu::kNo);
#endif

    auto snapshot = surface->makeImageSnapshot();

    SkAutoCanvasRestore autoRestore(&canvas, true);
    canvas.setMatrix(SkM44());

    // The surface is in device space and drawn at the same size, so pixels map 1:1.
    const SkSamplingOptions sampling(SkFilterMode::kNearest, SkMipmapMode::kNone);
    drawImageRectRestricted(canvas, context.damageRegionOrNull(), snapshot.get(), SkRect::MakeWH(surfaceRect.width(), surfaceRect.height()),
        SkRect::Make(surfaceRect), sampling, paint);
}

static std::optional<TransformationMatrix> inverseLayerPlaneTransform(const TransformationMatrix& layerTransform)
{
    return layerTransform.to2dTransform().inverse();
}

static int maxFilterSurfaceSize()
{
    auto* context = PlatformDisplay::sharedDisplay().skiaGrContext();
#if PLATFORM(SCARLET)
    return std::min(context->maxTextureSize(), context->maxRenderTargetSize());
#else
    return context->maxTextureSize() / 2;
#endif
}

template<typename BoundsFunction>
static FloatRect filterSourceBounds(const SkCanvas& canvas, const TransformationMatrix& layerTransform, const FloatSize& scale, const SkImageFilter& filter, NOESCAPE const BoundsFunction& boundsWithinClip)
{
    const auto deviceToLayer = inverseLayerPlaneTransform(TransformationMatrix(canvas.getLocalToDevice()).multiply(layerTransform));
    if (!deviceToLayer)
        return { };

    const auto deviceClip = FloatRect(canvas.getDeviceClipBounds());
    const auto clipBounds = layerPlaneClipBounds(*deviceToLayer, deviceClip);
    if (clipBounds.isEmpty())
        return { };

#if PLATFORM(SCARLET)
    UNUSED_PARAM(scale);
#else
    const int maxSurfaceSize = maxFilterSurfaceSize();
    auto fitsSurface = [&](const FloatRect& bounds) {
        return bounds.width() * scale.width() <= maxSurfaceSize && bounds.height() * scale.height() <= maxSurfaceSize;
    };
#endif

    // Like Chromium, also include the content outside the visible part that the filter reads, for example
    // content whose drop shadow falls into the visible part. If that is too large, use the visible part only.
    const auto reach = filter.filterBounds(clipBounds, SkMatrix::I(), SkImageFilter::kReverse_MapDirection);
    if (reach.width64() <= std::numeric_limits<int>::max() && reach.height64() <= std::numeric_limits<int>::max()) {
        const FloatRect bounds = boundsWithinClip(IntRect(reach));
#if PLATFORM(SCARLET)
        // Preserve every input pixel the visible filter output can read.
        // Oversized inputs are fitted uniformly below, never cropped here.
        return bounds;
#else
        if (fitsSurface(bounds))
            return bounds;
#endif
    }

    const FloatRect bounds = boundsWithinClip(clipBounds);
#if PLATFORM(SCARLET)
    return bounds;
#else
    if (fitsSurface(bounds))
        return bounds;

    // Within a smaller clip the source covers at most its intersection with the bounds above.
    return boundsWithinClip(layerPlaneClipBounds(*deviceToLayer, deviceClip, [&](const IntRect& clipBounds) {
        return fitsSurface(intersection(bounds, FloatRect(clipBounds)));
    }));
#endif
}

void SkiaCompositingLayer::paintBackdrop(SkCanvas& canvas, PaintContext& context)
{
#if ENABLE(DAMAGE_TRACKING)
    if (!context.shouldDraw()) {
        collectBackdropDamage(canvas, context);
        return;
    }
#endif

    context.imageSetBatch.flushIfNeeded(canvas);

    SkAutoCanvasRestore autoRestore(&canvas, true);
    const auto clipTransform = combinedTransform(context);
    if (m_backdrop.clipPath)
        canvas.clipPath(m_backdrop.clipPath->makeTransform(SkM44(clipTransform).asM33()), true);
    else
        clipRect(canvas, m_backdrop.clipRect, clipTransform);

    // Paint the backdrop root's subtree into a fresh surface (spec step 1),
    // apply the backdrop filter (step 2), and composite via SrcOver so the
    // filtered result blends onto the canvas without destroying ancestor
    // backgrounds that aren't part of the backdrop root's subtree.
    //
    // Use paintSelfAndChildren (not recursivePaint) on the backdrop root to
    // exclude the root's own effects (replica, filter, mask) per the CSS spec.
    auto paintBackdropRootSubtree = [&](SkCanvas& canvas, PaintContext& context) {
        SetForScope scopedPaintBackdropForLayer(context.paintingBackdropForLayer, this);
        SetForScope scopedOpacity(context.opacity, 1.f);
        SetForScope scopedBlendMode(context.blendMode, std::nullopt);
        SetForScope scopedReplicaTransform(context.accumulatedReplicaTransform, TransformationMatrix());
        SetForScope scopedSkipAfterBackdrop(context.skipAfterBackdrop, false);
        SetForScope scopedColorFilter(context.colorFilter, nullptr);
        backdropRoot()->paintSelfAndChildren(canvas, context);
    };

    // A layer plane that maps to zero area paints nothing - so we can skip the subtree paint.
    const auto inverseTransform = inverseLayerPlaneTransform(clipTransform);
    if (!inverseTransform)
        return;

    // The backdrop covers what the filter reads for the visible part, within the border box.
    const auto localBounds = filterSourceBounds(canvas, clipTransform, filterSurfaceScale(context), *m_backdrop.filter, [&](const IntRect& clipBounds) {
        return intersection(m_backdrop.clipRect.rect(), FloatRect(clipBounds));
    });

    SkPaint paint;
    paint.setAlphaf(context.opacity);
    paint.setImageFilter(SkiaCompositingLayerFilters::create(m_backdrop.filterOperations, SkTileMode::kMirror, SkRect(localBounds)));
    paint.setColorFilter(context.colorFilter);
    if (context.blendMode)
        paint.setBlendMode(*context.blendMode);
    // Per spec, a backdrop blur mirrors the image at the border box, and an aligned surface could include pixels from outside it.
    // https://drafts.csswg.org/filter-effects-2/#BackdropFilterProperty
    paintWithFilter(canvas, context, clipTransform, *inverseTransform, localBounds, paint, FilterSurfaceAlignment::LocalCoordinates, WTF::move(paintBackdropRootSubtree));
}

void SkiaCompositingLayer::paintWithMaskAndBackdrop(SkCanvas& canvas, PaintContext& context)
{
    // An empty clip path clips the whole layer away, so skip it in every mode.
    if (m_mask && m_mask->m_clipPath && m_mask->m_clipPath->isEmpty())
        return;

#if ENABLE(DAMAGE_TRACKING)
    // The mask only affects the drawn result, so apply it in the draw pass only. Damage collection
    // and debug indicators walk the tree unmasked.
    if (!context.shouldDraw()) {
        collectGroupDamage(canvas, context);

        if (m_backdrop.filter && !hasFilter())
            paintBackdrop(canvas, context);

        paintWithBlendMode(canvas, context);
        return;
    }
#endif

    bool shouldClipPath = false;
    sk_sp<SkImage> maskImage;
    if (m_mask) {
        shouldClipPath = m_mask->m_clipPath.has_value();
        if (!shouldClipPath)
            maskImage = m_mask->maskImage();
    }

    ScopedFlush autoFlush(canvas, context.imageSetBatch, shouldClipPath || maskImage ? ScopedFlush::Mode::FlushBeforeAndAfter : ScopedFlush::Mode::DoNothing);
    if (shouldClipPath || maskImage) {
        TransformationMatrix transform(context.accumulatedReplicaTransform);
        transform.multiply(m_mask->m_transforms.combined);
        if (maskImage)
            transform = transform.scale(1 / m_mask->m_backingStore->scale());
        auto matrix = SkM44(transform).asM33();

        if (shouldClipPath)
            canvas.clipPath(m_mask->m_clipPath->makeTransform(matrix), true);
        else if (auto maskShader = maskImage->makeShader(SkiaUtilities::samplingOptionsForMatrix(SkMatrix::Concat(canvas.getLocalToDeviceAs3x3(), matrix)), &matrix))
            canvas.clipShader(maskShader);
    }

    // A filter of the layer applies to its backdrop too, so paintWithFilterAndMask() paints the backdrop then.
    if (m_backdrop.filter && !hasFilter())
        paintBackdrop(canvas, context);

    paintWithBlendMode(canvas, context);
}

static std::optional<SkMatrix> computeDeviceAlignedSurfaceTransform(const TransformationMatrix& layerToDevice, const FloatRect& localBounds)
{
    auto matrix = SkM44(layerToDevice).asM33();
    matrix.normalizePerspective();
    if (!matrix.isScaleTranslate())
        return std::nullopt;

    const auto deviceBounds = matrix.mapRect(localBounds);
    const int maxSurfaceSize = maxFilterSurfaceSize();
    if (deviceBounds.width() > maxSurfaceSize || deviceBounds.height() > maxSurfaceSize)
        return std::nullopt;

    return matrix;
}

#if PLATFORM(SCARLET)
struct ScarletFilterSurfaceGeometry {
    FloatSize scale;
    std::optional<SkMatrix> deviceAlignedTransform;
    SkMatrix layerToScaled;
    IntRect sourceRect;
    IntRect outputRect;
    sk_sp<SkImageFilter> filter;
};

// Check before narrowing: saturating integer enclosure would silently change
// a finite out-of-range origin while apparently preserving its small extent.
static std::optional<std::array<int, 4>> scarletFilterIntegerBounds(double left, double top, double right, double bottom)
{
    left = std::floor(left);
    top = std::floor(top);
    right = std::ceil(right);
    bottom = std::ceil(bottom);
    constexpr double min = std::numeric_limits<int>::min();
    constexpr double max = std::numeric_limits<int>::max();
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom)
        || left < min || top < min || right > max || bottom > max
        || right <= left || bottom <= top || right - left > max || bottom - top > max)
        return std::nullopt;
    return std::array<int, 4> { static_cast<int>(left), static_cast<int>(top),
        static_cast<int>(right - left), static_cast<int>(bottom - top) };
}
static std::optional<IntRect> scarletEnclosingFilterRect(const SkRect& rect)
{
    auto bounds = scarletFilterIntegerBounds(rect.left(), rect.top(), rect.right(), rect.bottom());
    if (!bounds)
        return std::nullopt;
    return IntRect { (*bounds)[0], (*bounds)[1], (*bounds)[2], (*bounds)[3] };
}
static float scarletFilterCoordinateFit(const SkRect& rect)
{
    const double extent = std::max({ std::abs(static_cast<double>(rect.left())), std::abs(static_cast<double>(rect.top())),
        std::abs(static_cast<double>(rect.right())), std::abs(static_cast<double>(rect.bottom())), 1.0 });
    // Refit the existing layer-plane scale instead of clamping an invalid
    // origin. Half the integer range leaves space for outward rounding.
    return static_cast<float>((std::numeric_limits<int>::max() / 2.0) / extent);
}

// Fit both the full input and the visible output before allocating any GPU
// texture. The existing layer-plane path maps a smaller surface back to its
// original geometry, so an oversized filter loses resolution rather than area.
static std::optional<ScarletFilterSurfaceGeometry> scarletFilterSurfaceGeometry(
    const FloatRect& localBounds, const FloatRect& localClip, FloatSize scale,
    std::optional<SkMatrix> deviceAlignedTransform, const SkImageFilter& originalFilter, int maxSize)
{
    // Rounding a fractional origin and extent can add two pixels.
    const float available = maxSize - 2;
    if (!(available > 0) || !(scale.width() > 0) || !(scale.height() > 0)
        || !std::isfinite(scale.width()) || !std::isfinite(scale.height()))
        return std::nullopt;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        auto layerToScaled = deviceAlignedTransform.value_or(SkMatrix::Scale(scale.width(), scale.height()));
        const SkRect scaledBounds = layerToScaled.mapRect(SkRect(localBounds));
        if (!scaledBounds.isFinite() || scaledBounds.isEmpty())
            return std::nullopt;
        float ratio = std::min(available / scaledBounds.width(), available / scaledBounds.height());
        const auto enclosedSource = scarletEnclosingFilterRect(scaledBounds);
        if (ratio >= 1 && enclosedSource) {
            const IntRect sourceRect = *enclosedSource;
            auto layerToSurface = layerToScaled;
            layerToSurface.postTranslate(-static_cast<float>(sourceRect.x()), -static_cast<float>(sourceRect.y()));
            auto filter = originalFilter.makeWithLocalMatrix(layerToSurface);
            if (!filter)
                return std::nullopt;
            const auto sourceBounds = SkRect::MakeIWH(sourceRect.width(), sourceRect.height());
            auto outputBounds = filter->computeFastBounds(sourceBounds);
            auto clip = layerToSurface.mapRect(SkRect(localClip));
            if (!outputBounds.isFinite() || !clip.isFinite() || !outputBounds.intersect(clip))
                return std::nullopt;
            ratio = std::min(available / outputBounds.width(), available / outputBounds.height());
            if (ratio >= 1) {
                if (const auto outputRect = scarletEnclosingFilterRect(outputBounds)) {
                    if (sourceRect.width() <= maxSize && sourceRect.height() <= maxSize
                        && outputRect->width() <= maxSize && outputRect->height() <= maxSize)
                        return ScarletFilterSurfaceGeometry { scale, deviceAlignedTransform, layerToScaled, sourceRect, *outputRect, WTF::move(filter) };
                } else
                    ratio = scarletFilterCoordinateFit(outputBounds);
            }
        } else if (ratio >= 1)
            ratio = scarletFilterCoordinateFit(scaledBounds);
        if (!(ratio > 0) || !std::isfinite(ratio))
            return std::nullopt;
        // Keep the same aspect ratio and filter coordinates. A tiny safety
        // reduction also guarantees progress at a floating-point boundary.
        ratio = std::min(ratio, .99f);
        scale.scale(ratio);
        deviceAlignedTransform.reset();
    }
    return std::nullopt;
}
#endif

void SkiaCompositingLayer::paintWithFilter(SkCanvas& canvas, PaintContext& context, const TransformationMatrix& layerTransform, const TransformationMatrix& inverseLayerTransform, const FloatRect& localBounds, const SkPaint& layerPaint, FilterSurfaceAlignment alignment, NOESCAPE const PaintFunction& paintFunction)
{
    // Like Chromium, paint the subtree into a surface in the layer plane at a fixed scale, filter it there and
    // draw the result with the layer transform. SkCanvas::saveLayer() would pick the layer resolution at the
    // center of the bounds instead, which under a perspective can be far off the resolution anywhere else.
    auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
    auto scale = filterSurfaceScale(context);

    // Align the surface with the device pixels if possible, so the result needs no resampling.
    auto deviceAlignedTransform = alignment == FilterSurfaceAlignment::DevicePixels ? computeDeviceAlignedSurfaceTransform(TransformationMatrix(canvas.getLocalToDevice()).multiply(layerTransform), localBounds) : std::nullopt;
#if PLATFORM(SCARLET)
    const auto deviceToLayer = inverseLayerPlaneTransform(TransformationMatrix(canvas.getLocalToDevice()).multiply(layerTransform));
    const auto localClip = deviceToLayer ? FloatRect(layerPlaneClipBounds(*deviceToLayer, FloatRect(canvas.getDeviceClipBounds()))) : FloatRect();
    auto geometry = scarletFilterSurfaceGeometry(localBounds, localClip, scale, deviceAlignedTransform, *layerPaint.refImageFilter(), maxFilterSurfaceSize());
    if (!geometry)
        return;
    scale = geometry->scale;
    deviceAlignedTransform = geometry->deviceAlignedTransform;
    const auto layerToScaled = geometry->layerToScaled;
    const auto surfaceRect = geometry->sourceRect;
#else
    const auto layerToScaled = deviceAlignedTransform.value_or(SkMatrix::Scale(scale.width(), scale.height()));
    const FloatRect scaledBounds = layerToScaled.mapRect(localBounds);

    // A surface larger than that loses what is beyond it, as in Chromium.
    const int maxSurfaceSize = maxFilterSurfaceSize();
    auto surfaceRect = enclosingIntRect(scaledBounds);
    surfaceRect.setWidth(std::min(surfaceRect.width(), maxSurfaceSize));
    surfaceRect.setHeight(std::min(surfaceRect.height(), maxSurfaceSize));
    if (surfaceRect.isEmpty())
        return;
#endif

    auto imageInfo = SkImageInfo::Make(surfaceRect.width(), surfaceRect.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
#if PLATFORM(SCARLET)
    if (auto& profile = scarletScenePaintProfile(); profile.enabled)
        ScarletScenePaintProfile::add(profile.filterSurfaces, 1);
#endif
    auto surface = SkSurfaces::RenderTarget(grContext, skgpu::Budgeted::kNo, imageInfo, 0, kTopLeft_GrSurfaceOrigin, nullptr);
    if (!surface)
        return;

    auto* surfaceCanvas = surface->getCanvas();
    if (!surfaceCanvas)
        return;

    context.imageSetBatch.flushIfNeeded(canvas);

    surfaceCanvas->clear(SK_ColorTRANSPARENT);
    surfaceCanvas->translate(-static_cast<float>(surfaceRect.x()), -static_cast<float>(surfaceRect.y()));
    surfaceCanvas->concat(layerToScaled);
    surfaceCanvas->concat(SkM44(inverseLayerTransform));

    {
        // The filter may sample outside the damage, so paint the whole subtree and limit only the composite.
        SetForScope scopedNoDamageRestriction(context.compositingDamageRegion, std::nullopt);
        paintFunction(*surfaceCanvas, context);
        context.imageSetBatch.flushIfNeeded(*surfaceCanvas);
    }

#if PLATFORM(SCARLET)
    const auto sourceBounds = SkRect::MakeIWH(surfaceRect.width(), surfaceRect.height());
    auto filter = geometry->filter;
    const auto outputRect = geometry->outputRect;
#else
    // Filter parameters are in layer units, the surface is in scaled pixels relative to its origin.
    auto layerToSurface = layerToScaled;
    layerToSurface.postTranslate(-static_cast<float>(surfaceRect.x()), -static_cast<float>(surfaceRect.y()));
    auto filter = layerPaint.refImageFilter()->makeWithLocalMatrix(layerToSurface);

    // The filter output only has to cover the visible part of the layer plane, and has to fit in a texture.
    const auto sourceBounds = SkRect::MakeIWH(surfaceRect.width(), surfaceRect.height());
    const auto deviceToLayer = inverseLayerPlaneTransform(TransformationMatrix(canvas.getLocalToDevice()).multiply(layerTransform));
    auto clipRect = deviceToLayer ? FloatRect(layerPlaneClipBounds(*deviceToLayer, FloatRect(canvas.getDeviceClipBounds()))) : FloatRect();
    clipRect = layerToScaled.mapRect(clipRect);
    clipRect.move(-static_cast<float>(surfaceRect.x()), -static_cast<float>(surfaceRect.y()));
    auto outputRect = intersection(enclosingIntRect(FloatRect(filter->computeFastBounds(sourceBounds))), enclosingIntRect(clipRect));
    outputRect.setWidth(std::min(outputRect.width(), maxSurfaceSize));
    outputRect.setHeight(std::min(outputRect.height(), maxSurfaceSize));
    if (outputRect.isEmpty())
        return;
#endif

    SkIRect filteredSubset;
    SkIPoint filteredOffset;
    auto filtered = SkImages::MakeWithFilter(grContext, surface->makeImageSnapshot(), filter.get(), sourceBounds.roundOut(), SkIRect(outputRect), &filteredSubset, &filteredOffset);
    if (!filtered)
        return;

    {
        SkAutoCanvasRestore autoRestore(&canvas, true);

        if (const auto* damageRegion = context.damageRegionOrNull())
            damageRegion->clipCanvasInDeviceSpace(canvas);

        if (deviceAlignedTransform)
            canvas.setMatrix(SkM44::Translate(surfaceRect.x(), surfaceRect.y()));
        else {
            canvas.concat(SkM44(layerTransform));
            canvas.scale(1 / scale.width(), 1 / scale.height());
            canvas.translate(surfaceRect.x(), surfaceRect.y());
        }

        SkPaint paint(layerPaint);
        paint.setImageFilter(nullptr);
        paint.setAntiAlias(true);
        const auto destinationRect = SkRect::MakeXYWH(filteredOffset.x(), filteredOffset.y(), filteredSubset.width(), filteredSubset.height());
        const auto sampling = SkSamplingOptions(deviceAlignedTransform ? SkFilterMode::kNearest : SkFilterMode::kLinear);
        canvas.drawImageRect(filtered.get(), SkRect::Make(filteredSubset), destinationRect, sampling, &paint, SkCanvas::kStrict_SrcRectConstraint);
    }

#if !PLATFORM(SCARLET)
    // The render tasks are now recorded -- submit them to the GPU now, instead of
    // waiting for the final flushAndSubmit() once the full layer tree finished painting.
    grContext->flush();
#endif
}

static std::optional<FloatSize> transform2DScaleComponents(const TransformationMatrix& transform)
{
    if (transform.m14() || transform.m24() || !std::isnormal(transform.m44()))
        return std::nullopt;

    const double w = std::abs(transform.m44());
    return FloatSize(std::hypot(transform.m11(), transform.m12(), transform.m13()) / w, std::hypot(transform.m21(), transform.m22(), transform.m23()) / w);
}

FloatSize SkiaCompositingLayer::filterSurfaceScale(const PaintContext& context) const
{
    // A perspective has no single scale, so fall back to the nearest ancestor that has one, which includes the
    // device and page scale. Chromium falls back to the device and page scale the same way.
    if (auto scale = transform2DScaleComponents(combinedTransform(context)))
        return *scale;

    for (RefPtr layer = m_parent.get(); layer; layer = layer->m_parent.get()) {
        if (auto scale = transform2DScaleComponents(layer->m_transforms.combined))
            return *scale;
    }
    return { 1, 1 };
}

void SkiaCompositingLayer::paintWithFilterAndMask(SkCanvas& canvas, PaintContext& context)
{
    auto filter = this->filter();
    if (!filter || !filter->filter) {
        paintSelfAndChildren(canvas, context);
        return;
    }

    auto paintBackdropSelfAndChildren = [&](SkCanvas& canvas, PaintContext& context) {
        if (m_backdrop.filter)
            paintBackdrop(canvas, context);
        paintSelfAndChildren(canvas, context);
    };

    // If we have a filter that can be simplified as a color filter
    // we don't need to create an intermediate surface.
    SkColorFilter* colorFilterPtr = nullptr;
    if (filter->filter->asAColorFilter(&colorFilterPtr)) {
        sk_sp<SkColorFilter> colorFilter(colorFilterPtr);
        SetForScope scopedColorFilter(context.colorFilter, colorFilter);
        paintBackdropSelfAndChildren(canvas, context);
        return;
    }

    // We re-use the logic for the overlap region computation to compute the local bounds of the
    // whole subtree, which size the surface the filter is applied in. It has to cover the visible area.
    const auto layerTransform = combinedTransform(context);
    const auto inverseTransform = inverseLayerPlaneTransform(layerTransform);
    auto computeLocalBounds = [&](const IntRect& clipBounds) {
        ComputeOverlapRegionData data {
            .mode = ComputeOverlapRegionMode::Union,
            .clipBounds = clipBounds,
            .overlapRegion = { },
            .nonOverlapRegion = { },
            .canvasTransform = *inverseTransform
        };
        computeOverlapRegions(data, context.accumulatedReplicaTransform, IncludesReplica::No, IncludesFilterOutsets::No);
        return FloatRect(data.overlapRegion.bounds());
    };

#if ENABLE(DAMAGE_TRACKING)
    if (!context.shouldDraw()) {
        // A filter also reads pixels from outside the layer. So if anything in the subtree has changed,
        // the whole overlap region has changed, not only the part that has been changed. If nothing has
        // changed, the region still matches the last frame and no damage is needed. Changes to this
        // layer's own group properties are left to collectGroupDamage().
        if (!hasGroupPropertyDamage() && hasDamageInSubtree()) {
            auto mode = m_mask ? ComputeOverlapRegionMode::Mask : ComputeOverlapRegionMode::Union;
            auto overlapRects = computeConsolidatedOverlapRegionRects(canvas, context, mode);

            if (inverseTransform) {
                auto outputBounds = filterSourceBounds(canvas, layerTransform, filterSurfaceScale(context), *filter->filter, computeLocalBounds);
                if (!outputBounds.isEmpty()) {
                    outputBounds.expand(toFloatBoxExtent(filter->outsets));
                    overlapRects.append(projectedBoundingBox(TransformationMatrix(canvas.getLocalToDevice()).multiply(layerTransform), outputBounds, canvas.getDeviceClipBounds()));
                }
            }

            addGroupDamage(canvas, context, overlapRects);
        }

        paintBackdropSelfAndChildren(canvas, context);
        return;
    }
#endif

    if (!inverseTransform)
        return;

    const auto localBounds = filterSourceBounds(canvas, layerTransform, filterSurfaceScale(context), *filter->filter, computeLocalBounds);

    SkPaint layerPaint;
    layerPaint.setImageFilter(filter->filter);

    auto paintFilteredSubtree = [&](SkCanvas& canvas, PaintContext& context) {
        paintWithFilter(canvas, context, layerTransform, *inverseTransform, localBounds, layerPaint, FilterSurfaceAlignment::DevicePixels, [&](SkCanvas& canvas, PaintContext& context) {
            paintBackdropSelfAndChildren(canvas, context);
        });
    };

    if (!m_mask) {
        paintFilteredSubtree(canvas, context);
        return;
    }

    for (const auto& rect : computeConsolidatedOverlapRegionRects(canvas, context, ComputeOverlapRegionMode::Mask)) {
        paintWithIntermediateSurface(canvas, context, rect, nullptr, [&](SkCanvas& canvas, PaintContext& context) {
            paintFilteredSubtree(canvas, context);
        });
    }
}

Vector<IntRect, 1> SkiaCompositingLayer::computeConsolidatedOverlapRegionRects(const SkCanvas& canvas, const PaintContext& context, ComputeOverlapRegionMode mode)
{
    ComputeOverlapRegionData data {
        .mode = mode,
        .clipBounds = canvas.getDeviceClipBounds(),
        .overlapRegion = { },
        .nonOverlapRegion = { },
        .canvasTransform = canvas.getLocalToDevice()
    };
    computeOverlapRegions(data, context.accumulatedReplicaTransform, IncludesReplica::No);

    auto rects = data.overlapRegion.rects();
    if (rects.size() > cOverlapRegionConsolidationThreshold) {
        rects.clear();
        rects.append(data.overlapRegion.bounds());
    }

    return rects;
}

void SkiaCompositingLayer::paintWithReplica(SkCanvas& canvas, PaintContext& context)
{
    if (m_replica) {
        auto newAccumulatedReplicaTransform = TransformationMatrix(context.accumulatedReplicaTransform).multiply(replicaTransform());
        SetForScope scopedReplicaTransform(context.accumulatedReplicaTransform, newAccumulatedReplicaTransform);
        paintWithMaskAndBackdrop(canvas, context);
    }

    paintWithMaskAndBackdrop(canvas, context);
}

void SkiaCompositingLayer::recursivePaint(SkCanvas& canvas, PaintContext& context)
{
    if (context.skipAfterBackdrop)
        return;
    if (stopPaintingIntoBackdropIfNeeded(context))
        return;
    if (!isVisible())
        return;

    SetForScope scopedOpacity(context.opacity, context.opacity * opacity());

    if (m_preserves3D)
        paintWith3DRenderingContext(canvas, context);
    else
        paintWithOpacity(canvas, context);

#if ENABLE(DAMAGE_TRACKING)
    if (!context.shouldDraw())
        return;
#endif

    if (m_repaintCount)
        paintRepaintCounter(canvas, context);
}

void SkiaCompositingLayer::computeOverlapRegions(ComputeOverlapRegionData& data, const TransformationMatrix& accumulatedReplicaTransform, IncludesReplica includesReplica, IncludesFilterOutsets includesFilterOutsets)
{
    if (!m_visible || !m_contentsVisible)
        return;

    auto filter = this->filter();

    FloatRect localBoundingRect;
    if (m_backingStore || m_masksToBounds || m_mask || filter || m_backdrop.filter)
        localBoundingRect = m_rect;
    else if (paintsContentsRect())
        localBoundingRect = m_contentsRect;

    if (includesFilterOutsets == IncludesFilterOutsets::Yes)
        localBoundingRect.expand(toFloatBoxExtent(unclippedFilterOutsets()));

    TransformationMatrix transform(data.canvasTransform);
    transform.multiply(accumulatedReplicaTransform);
    transform.multiply(m_transforms.combined);

    const auto viewportBoundingRect = projectedBoundingBox(transform, localBoundingRect, data.clipBounds);
    switch (data.mode) {
    case ComputeOverlapRegionMode::Intersection:
        data.resolveOverlaps(viewportBoundingRect);
        break;
    case ComputeOverlapRegionMode::Union:
    case ComputeOverlapRegionMode::Mask:
        data.overlapRegion.unite(viewportBoundingRect);
        break;
    }

    if (m_replica && includesReplica == IncludesReplica::Yes) {
        TransformationMatrix newReplicaTransform(accumulatedReplicaTransform);
        newReplicaTransform.multiply(replicaTransform());
        computeOverlapRegions(data, newReplicaTransform, IncludesReplica::No, includesFilterOutsets);
    }

    if (!m_masksToBounds && data.mode != ComputeOverlapRegionMode::Mask) {
        for (auto& child : m_children)
            child->computeOverlapRegions(data, accumulatedReplicaTransform);
    }
}

void SkiaCompositingLayer::paintWithOpacity(SkCanvas& canvas, PaintContext& context)
{
    if (opacity() == 1) {
        paintWithReplica(canvas, context);
        return;
    }

#if ENABLE(DAMAGE_TRACKING)
    // The overlap regions below only exist to composite the group correctly, and the collecting walk
    // composites nothing. Splitting there would narrow the clip, shrink the damage each sub-walk collects
    // and walk the subtree once per region rect. One unsplit walk collects a superset of the same rects.
    if (!context.shouldDraw()) {
        paintWithReplica(canvas, context);
        return;
    }
#endif

    ComputeOverlapRegionData data {
        .mode = ComputeOverlapRegionMode::Intersection,
        .clipBounds = canvas.getDeviceClipBounds(),
        .overlapRegion = { },
        .nonOverlapRegion = { },
        .canvasTransform = canvas.getLocalToDevice()
    };
    computeOverlapRegions(data, context.accumulatedReplicaTransform);

    if (data.overlapRegion.isEmpty()) {
        paintWithReplica(canvas, context);
        return;
    }

    // Having both overlap and non-overlap regions carries some overhead.
    // Avoid it if the overlap area is big anyway.
    if (data.overlapRegion.totalArea() > data.nonOverlapRegion.totalArea()) {
        data.overlapRegion.unite(data.nonOverlapRegion);
        data.nonOverlapRegion = Region();
    }

    for (const auto& rect : data.nonOverlapRegion.rects()) {
        ScopedFlush autoFlush(canvas, context.imageSetBatch, ScopedFlush::Mode::FlushBeforeAndAfter);
        clipRectIgnoringCanvasMatrix(canvas, rect);
        paintWithReplica(canvas, context);
    }

    auto overlapRects = data.overlapRegion.rects();
    if (data.nonOverlapRegion.isEmpty() && overlapRects.size() > cOverlapRegionConsolidationThreshold) {
        overlapRects.clear();
        overlapRects.append(data.overlapRegion.bounds());
    }

    SkPaint layerPaint;
    layerPaint.setAlphaf(context.opacity);
    for (const auto& rect : overlapRects) {
        SkAutoCanvasRestore autoRestore(&canvas, true);
        paintWithIntermediateSurface(canvas, context, rect, &layerPaint, [&](SkCanvas& canvas, PaintContext& context) {
            SetForScope scopedOpacity(context.opacity, 1);
            paintWithReplica(canvas, context);
        });
    }
}

void SkiaCompositingLayer::paintWithBlendMode(SkCanvas& canvas, PaintContext& context)
{
    if (!m_shouldBlend) {
        paintWithFilterAndMask(canvas, context);
        return;
    }

#if ENABLE(DAMAGE_TRACKING)
    // See paintWithOpacity(): the collecting walk composites nothing, so it needs neither the blend mode
    // nor the overlap regions.
    if (!context.shouldDraw()) {
        paintWithFilterAndMask(canvas, context);
        return;
    }
#endif

    auto blendMode = m_blendMode;
    if (!blendMode && m_shouldBlend)
        blendMode = SkBlendMode::kSrcOver;
    SetForScope scopedBlendMode(context.blendMode, context.blendMode ? context.blendMode : blendMode);

    ComputeOverlapRegionData data {
        .mode = ComputeOverlapRegionMode::Intersection,
        .clipBounds = canvas.getDeviceClipBounds(),
        .overlapRegion = { },
        .nonOverlapRegion = { },
        .canvasTransform = canvas.getLocalToDevice()
    };
    computeOverlapRegions(data, context.accumulatedReplicaTransform);

    if (data.overlapRegion.isEmpty()) {
        paintWithFilterAndMask(canvas, context);
        return;
    }

    // Having both overlap and non-overlap regions carries some overhead.
    // Avoid it if the overlap area is big anyway.
    if (data.overlapRegion.totalArea() > data.nonOverlapRegion.totalArea()) {
        data.overlapRegion.unite(data.nonOverlapRegion);
        data.nonOverlapRegion = Region();
    }

    for (const auto& rect : data.nonOverlapRegion.rects()) {
        ScopedFlush autoFlush(canvas, context.imageSetBatch, ScopedFlush::Mode::FlushBeforeAndAfter);
        clipRectIgnoringCanvasMatrix(canvas, rect);
        paintWithFilterAndMask(canvas, context);
    }

    auto overlapRects = data.overlapRegion.rects();
    if (data.nonOverlapRegion.isEmpty() && overlapRects.size() > cOverlapRegionConsolidationThreshold) {
        overlapRects.clear();
        overlapRects.append(data.overlapRegion.bounds());
    }

    SkPaint layerPaint;
    if (context.blendMode)
        layerPaint.setBlendMode(*context.blendMode);
    for (const auto& rect : overlapRects) {
        SkAutoCanvasRestore autoRestore(&canvas, true);
        paintWithIntermediateSurface(canvas, context, rect, &layerPaint, [&](SkCanvas& canvas, PaintContext& context) {
            SetForScope scopedBlendMode(context.blendMode, std::nullopt);
            paintWithFilterAndMask(canvas, context);
        });
    }
}

FloatRect SkiaCompositingLayer::transformedFlattenedBounds() const
{
    auto bounds = m_transforms.combined.mapRect(m_rect);

    if (!m_masksToBounds && !m_mask) {
        for (const auto& child : m_children)
            bounds.unite(child->transformedFlattenedBounds());
    }

    return bounds;
}

Polygon4D SkiaCompositingLayer::geometryFor3DRenderingContext() const
{
    FloatRect bounds = m_rect;
    if (bounds.isEmpty() && isLeafOf3DRenderingContext() && !m_children.isEmpty() && !m_masksToBounds && !m_mask) {
        if (auto inverse = m_transforms.combined.inverse())
            bounds = inverse->mapRect(transformedFlattenedBounds());
    }

    return Polygon4D(bounds, m_transforms.combined);
}

void SkiaCompositingLayer::collect3DRenderingContextLayers(Vector<SkiaCompositingLayer3DRenderingContext::Layer>& layers)
{
    if (m_preserves3D || isLeafOf3DRenderingContext()) {
        // Add layers to 3d rendering context only if they get actually painted.
        bool hasVisualContentOrFilters = hasVisualContent() || filter() || m_backdrop.filter;
        if (isVisible() && (hasVisualContentOrFilters || (isLeafOf3DRenderingContext() && !m_children.isEmpty()))) {
            if (auto geometry = geometryFor3DRenderingContext(); geometry.numberOfVertices() >= 3)
                layers.append(SkiaCompositingLayer3DRenderingContext::Layer(Ref { *this }, WTF::move(geometry)));
        }

        // Stop recursion on 3d rendering context leaf
        if (isLeafOf3DRenderingContext())
            return;
    }

    for (const auto& child : m_children)
        child->collect3DRenderingContextLayers(layers);
}

void SkiaCompositingLayer::paintWith3DRenderingContext(SkCanvas& canvas, PaintContext& context)
{
    Vector<SkiaCompositingLayer3DRenderingContext::Layer> layers;
    collect3DRenderingContextLayers(layers);

    SkiaCompositingLayer3DRenderingContext::paint(WTF::move(layers), [&](SkiaCompositingLayer& layer, std::optional<SkPath> clipPath) {
        ScopedFlush autoFlush(canvas, context.imageSetBatch, clipPath ? ScopedFlush::Mode::FlushBeforeAndAfter : ScopedFlush::Mode::DoNothing);
        if (clipPath)
            canvas.clipPath(*clipPath);

        if (layer.m_preserves3D)
            layer.paintSelf(canvas, context);
        else
            layer.recursivePaint(canvas, context);
    });
}

void SkiaCompositingLayer::recursiveCleanUpAfterPaint()
{
#if ENABLE(DAMAGE_TRACKING)
    m_layerDamage = std::nullopt;
    m_groupPropertyChanged = false;

    // A mask and a replica are not children, and the walk does not reach them, so their damage would
    // otherwise pile up forever.
    if (m_mask)
        m_mask->recursiveCleanUpAfterPaint();
    if (m_replica)
        m_replica->recursiveCleanUpAfterPaint();

    for (auto& child : m_children)
        child->recursiveCleanUpAfterPaint();
#endif
}

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS) && USE(SKIA) && !USE(TEXTURE_MAPPER)
