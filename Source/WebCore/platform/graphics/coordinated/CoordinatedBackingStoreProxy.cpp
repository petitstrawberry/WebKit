/*
 * Copyright (C) 2010-2012 Nokia Corporation and/or its subsidiary(-ies)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include <wtf/HashSet.h>
#include <array>
#include <cmath>
#include "CoordinatedBackingStoreProxy.h"

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedAnimatedBackingStoreClient.h"
#include "CoordinatedPlatformLayer.h"
#include "CoordinatedTileBuffer.h"
#include "GraphicsLayerCoordinated.h"
#include "PlatformDisplay.h"
#include "ProcessCapabilities.h"
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/MemoryPressureHandler.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringToIntegerConversion.h>

#if USE(SKIA)
#include "SkiaPaintingEngine.h"
#include "SkiaRecordingResult.h"
#endif

#if USE(CAIRO)
#include "CairoPaintingContext.h"
#include "CairoPaintingEngine.h"
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CoordinatedBackingStoreProxy);

static uint32_t generateTileID()
{
    static uint32_t id = 0;
    // We may get a zero ID due to wrap-around on overflow.
    if (++id)
        return id;
    return ++id;
}

CoordinatedBackingStoreProxy::Update::~Update() = default;

void CoordinatedBackingStoreProxy::Update::appendUpdate(Vector<uint32_t>&& tilesToCreate, Vector<TileUpdate>&& tilesToUpdate, Vector<uint32_t>&& tilesToRemove)
{
    // Remove any creations or updates previously registered for tiles that are going to be removed now.
    if (!tilesToRemove.isEmpty() && (!m_tilesToCreate.isEmpty() || !m_tilesToUpdate.isEmpty())) {
        Vector<uint32_t, 8> createdTilesRemoved;
        for (const auto& tileID : tilesToRemove) {
            if (m_tilesToCreate.removeAll(tileID))
                createdTilesRemoved.append(tileID);
            m_tilesToUpdate.removeAllMatching([tileID](auto& update) {
                return update.tileID == tileID;
            });
        }

        // Remove the removals of tiles also registered to be created.
        for (const auto& tileID : createdTilesRemoved)
            tilesToRemove.removeFirst(tileID);
    }

    if (m_tilesToCreate.isEmpty())
        m_tilesToCreate = WTF::move(tilesToCreate);
    else
        m_tilesToCreate.appendVector(WTF::move(tilesToCreate));

    if (m_tilesToUpdate.isEmpty())
        m_tilesToUpdate = WTF::move(tilesToUpdate);
    else
        m_tilesToUpdate.appendVector(WTF::move(tilesToUpdate));

    if (m_tilesToRemove.isEmpty())
        m_tilesToRemove = WTF::move(tilesToRemove);
    else
        m_tilesToRemove.appendVector(WTF::move(tilesToRemove));
}

void CoordinatedBackingStoreProxy::Update::waitUntilPaintingComplete()
{
    for (auto& update : m_tilesToUpdate)
        update.buffer->waitUntilPaintingComplete();
}

Ref<CoordinatedBackingStoreProxy> CoordinatedBackingStoreProxy::create()
{
    return adoptRef(*new CoordinatedBackingStoreProxy());
}

CoordinatedBackingStoreProxy::~CoordinatedBackingStoreProxy()
{
    ASSERT(!m_animatedBackingStoreClient);
}

void CoordinatedBackingStoreProxy::invalidate()
{
    setAffectedByTransformAnimation(false);
}

void CoordinatedBackingStoreProxy::setAffectedByTransformAnimation(bool affectedByTransformAnimation)
{
    if (affectedByTransformAnimation && !m_animatedBackingStoreClient)
        m_animatedBackingStoreClient = CoordinatedAnimatedBackingStoreClient::create();
    else if (!affectedByTransformAnimation && m_animatedBackingStoreClient) {
        m_animatedBackingStoreClient->invalidate();
        m_animatedBackingStoreClient = nullptr;
    }
}

OptionSet<CoordinatedBackingStoreProxy::UpdateResult> CoordinatedBackingStoreProxy::updateIfNeeded(const IntRect& unscaledVisibleRect, const FloatSize& unscaledSize, const FloatRect& unscaledViewportRect, float contentsScale, bool contentsOpaque, bool shouldCreateAndDestroyTiles, const Vector<IntRect, 1>& dirtyRegion, Damage& damage, CoordinatedPlatformLayer& layer)
{
    assertIsMainThread();
    ASSERT(layer.owner());
    Vector<uint32_t> tilesToCreate;
    Vector<uint32_t> tilesToRemove;
    if (shouldCreateAndDestroyTiles)
        createOrDestroyTiles(unscaledVisibleRect, IntRect { { }, IntSize(unscaledSize) }, enclosingIntRect(unscaledViewportRect).size(), contentsScale, layer.maxTextureSize(), damage, tilesToCreate, tilesToRemove,
#if PLATFORM(SCARLET)
            layer.filterInputCoverage() ? &*layer.filterInputCoverage() : nullptr
#else
            nullptr
#endif
        );

    if (m_animatedBackingStoreClient)
        m_animatedBackingStoreClient->update(layer.owner(), unscaledViewportRect, m_coverRect, unscaledSize, contentsScale);

    if (!m_tiles.isEmpty())
        invalidateRegion(dirtyRegion);

    OptionSet<UpdateResult> result;
    if (m_pendingTileCreation)
        result.add(UpdateResult::TilesPending);

    // Update the dirty tiles.
    IntRect tileDirtyRectUnion;
    unsigned dirtyTilesCount = 0;
    for (const auto& tile : m_tiles.values()) {
        if (!tile.isDirty())
            continue;

        tileDirtyRectUnion.unite(tile.dirtyRect);
        ++dirtyTilesCount;
    }

    Vector<Update::TileUpdate> tilesToUpdate;
    if (dirtyTilesCount) {
        WTFBeginSignpost(this, UpdateTiles, "dirty tiles: %u", dirtyTilesCount);

        auto& paintingEngine = layer.client().paintingEngine();

#if USE(SKIA)
        // Record only once the whole layer.
        auto recording = paintingEngine.record(*layer.owner(), tileDirtyRectUnion, contentsOpaque, contentsScale, dirtyTilesCount);
#endif

        unsigned dirtyTileIndex = 0;
        for (auto& tile : m_tiles.values()) {
            if (!tile.isDirty())
                continue;

            WTFBeginSignpost(this, UpdateTile, "%u/%u, id: %d, rect: %ix%i+%i+%i, dirty: %ix%i+%i+%i", ++dirtyTileIndex, dirtyTilesCount, tile.id,
                tile.rect.x(), tile.rect.y(), tile.rect.width(), tile.rect.height(), tile.dirtyRect.x(), tile.dirtyRect.y(), tile.dirtyRect.width(), tile.dirtyRect.height());

#if USE(SKIA)
            auto buffer = paintingEngine.replay(*layer.owner(), recording.copyRef(), tile.rect, tile.dirtyRect);
#else
            FloatRect scaledDirtyRect(tile.dirtyRect);
            scaledDirtyRect.scale(1 / contentsScale);

            auto buffer = CoordinatedUnacceleratedTileBuffer::create(tile.dirtyRect.size(), contentsOpaque ? CoordinatedTileBuffer::NoFlags : CoordinatedTileBuffer::SupportsAlpha);
            paintingEngine.paint(*layer.owner(), buffer.get(), tile.dirtyRect, enclosingIntRect(scaledDirtyRect), IntRect { { }, tile.dirtyRect.size() }, contentsScale);
#endif

            IntRect updateRect(tile.dirtyRect);
            updateRect.move(-tile.rect.x(), -tile.rect.y());
            tilesToUpdate.append({ tile.id, tile.rect, WTF::move(updateRect), WTF::move(buffer) });
            tile.markClean();
            result.add(UpdateResult::BuffersChanged);

            WTFEndSignpost(this, UpdateTile);
        }

#if !HAVE(OS_SIGNPOST) && !USE(SYSPROF_CAPTURE)
        UNUSED_VARIABLE(dirtyTileIndex);
#endif

        WTFEndSignpost(this, UpdateTiles);
    }

#if !HAVE(OS_SIGNPOST) && !USE(SYSPROF_CAPTURE)
    UNUSED_VARIABLE(dirtyTilesCount);
#endif

    if (tilesToCreate.isEmpty() && tilesToUpdate.isEmpty() && tilesToRemove.isEmpty())
        return result;

    result.add(UpdateResult::TilesChanged);
    {
        Locker locker { m_update.lock };
        m_update.pending.appendUpdate(WTF::move(tilesToCreate), WTF::move(tilesToUpdate), WTF::move(tilesToRemove));
    }
    return result;
}

void CoordinatedBackingStoreProxy::invalidateRegion(const Vector<IntRect, 1>& dirtyRegion)
{
    for (const auto& contentsDirtyRect : dirtyRegion) {
        IntRect dirtyRect(mapFromContents(contentsDirtyRect));
        if (m_filterKeep) {
            // Existing tiles are sparse; never scan the gap in their bounds.
            for (auto& tile : m_tiles.values()) {
                if (tile.rect.intersects(dirtyRect))
                    tile.addDirtyRect(dirtyRect);
            }
            continue;
        }
        IntRect keepRectFitToTileSize = tileRectForPosition(tilePositionForPoint(m_keepRect.minXMinYCorner()));
        keepRectFitToTileSize.unite(tileRectForPosition(tilePositionForPoint(m_keepRect.maxXMaxYCorner() - IntSize(1, 1))));

        // Only iterate on the part of the rect that we know we might have tiles.
        IntRect coveredDirtyRect = intersection(dirtyRect, keepRectFitToTileSize);
        if (coveredDirtyRect.isEmpty())
            continue;
        forEachTilePositionInRect(coveredDirtyRect, [&](IntPoint&& position) {
            auto it = m_tiles.find(position);
            if (it == m_tiles.end())
                return;

            // Pass the full rect to each tile as coveredDirtyRect might not
            // contain them completely and we don't want partial tile redraws.
            it->value.addDirtyRect(dirtyRect);
        });
    }
}

// Expand in double precision, intersect with real contents, then narrow.
// In particular a tiny transform can request a billion-pixel input rectangle:
// its prefetch expansion must not overflow before the contents intersection.
static std::optional<std::array<int, 4>> boundedFilterTileBounds(
    double left, double top, double right, double bottom, double growX, double growY,
    const std::array<int, 4>& contents)
{
    const double clipLeft = contents[0], clipTop = contents[1];
    const double clipRight = clipLeft + contents[2], clipBottom = clipTop + contents[3];
    left = std::max(std::floor(left - growX), clipLeft);
    top = std::max(std::floor(top - growY), clipTop);
    right = std::min(std::ceil(right + growX), clipRight);
    bottom = std::min(std::ceil(bottom + growY), clipBottom);
    constexpr double min = std::numeric_limits<int>::min(), max = std::numeric_limits<int>::max();
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom)
        || left < min || top < min || right > max || bottom > max
        || right <= left || bottom <= top || right - left > max || bottom - top > max)
        return std::nullopt;
    return std::array<int, 4> { static_cast<int>(left), static_cast<int>(top),
        static_cast<int>(right - left), static_cast<int>(bottom - top) };
}
static IntRect boundedFilterTileRect(const IntRect& rect, double scale, double growX, double growY, const IntRect& contents)
{
    auto bounds = boundedFilterTileBounds(rect.x() * scale, rect.y() * scale,
        (static_cast<double>(rect.x()) + rect.width()) * scale,
        (static_cast<double>(rect.y()) + rect.height()) * scale, growX, growY,
        { contents.x(), contents.y(), contents.width(), contents.height() });
    return bounds ? IntRect { (*bounds)[0], (*bounds)[1], (*bounds)[2], (*bounds)[3] } : IntRect { };
}

void CoordinatedBackingStoreProxy::createOrDestroyTiles(const IntRect& unscaledVisibleRect, const IntRect& unscaledContentsRect, const IntSize& unscaledViewportSize, float contentsScale, int maxTextureSize, Damage& damage, Vector<uint32_t>& tilesToCreate, Vector<uint32_t>& tilesToRemove, const Region* filterInputCoverage)
{
    float coverAreaMultiplier = MemoryPressureHandler::singleton().isUnderMemoryPressure() ? 1.0f : 2.0f;
    bool contentsScaleChanged = m_contentsScale != contentsScale;
    m_contentsScale = contentsScale;

    IntRect contentsRect = mapFromContents(unscaledContentsRect);
    IntRect visibleRect = mapFromContents(unscaledVisibleRect);
    bool contentsRectChanged = contentsScaleChanged || m_contentsRect != contentsRect;
    std::optional<Region> inputCoverage;
    if (filterInputCoverage) {
        inputCoverage.emplace();
        for (auto rect : filterInputCoverage->rects())
            inputCoverage->unite(boundedFilterTileRect(rect, contentsScale, 0, 0, contentsRect));
    }
    bool geometryChanged = contentsRectChanged || m_visibleRect != visibleRect || m_coverAreaMultiplier != coverAreaMultiplier
        || m_filterInputCoverage != inputCoverage;
    if (!geometryChanged && !m_pendingTileCreation)
        return;

    // Recompute pending work for the current geometry. Empty contents or cover
    // below have no tiles left to create, even if the previous cover did.
    m_pendingTileCreation = false;

    if (geometryChanged) {
        m_contentsRect = contentsRect;
        m_visibleRect = visibleRect;
        m_filterInputCoverage = WTF::move(inputCoverage);
        m_coverAreaMultiplier = coverAreaMultiplier;

        bool tileSizeChanged = false;
        if (!m_contentsRect.isEmpty()) {
            auto tileSize = computeTileSize(unscaledViewportSize.scaled(m_contentsScale), maxTextureSize);
            tileSizeChanged = m_tileSize != tileSize;
            m_tileSize = WTF::move(tileSize);
        }

        if (tileSizeChanged || contentsScaleChanged || m_contentsRect.isEmpty()) {
            m_coverRect = { };
            m_keepRect = { };
            m_filterCover.reset();
            m_filterKeep.reset();
            if (!m_tiles.isEmpty()) {
                for (const auto& tile : m_tiles.values())
                    tilesToRemove.append(tile.id);
                m_tiles.clear();
            }

            if (m_contentsRect.isEmpty())
                return;
        }
    }

    /* We must compute cover and keep rects using the visibleRect, instead of the rect intersecting the visibleRect with m_contentsRect,
     * because TBS can be used as a backing store of GraphicsLayer and the visible rect usually does not intersect with m_contentsRect.
     * In the below case, the intersecting rect is an empty.
     *
     *  +----------------+
     *  |                |
     *  | m_contentsRect |
     *  |       +--------|----------------------+
     *  |       | HERE   |  cover or keep       |
     *  +----------------+      rect            |
     *          |         +---------+           |
     *          |         | visible |           |
     *          |         |  rect   |           |
     *          |         +---------+           |
     *          |                               |
     *          |                               |
     *          +-------------------------------+
     *
     * We must create or keep the tiles in the HERE region.
     */

    auto [coverRect, keepRect] = computeCoverAndKeepRect();
    m_coverRect = WTF::move(coverRect);
    m_keepRect = WTF::move(keepRect);
    m_filterCover.reset();
    m_filterKeep.reset();
    if (m_filterInputCoverage) {
        m_filterCover.emplace(m_coverRect);
        m_filterKeep.emplace(m_keepRect);
        for (auto input : m_filterInputCoverage->rects()) {
            // Retain the existing viewport-derived tile size and prefetch
            // heuristic for each island, without covering the gap between them.
            const auto cover = boundedFilterTileRect(input, 1,
                input.width() * static_cast<double>(m_coverAreaMultiplier - 1) / 2,
                input.height() * static_cast<double>(m_coverAreaMultiplier - 1) / 2, m_contentsRect);
            m_filterCover->unite(cover);
            const auto keep = boundedFilterTileRect(cover, 1, m_tileSize.width() / 2.0, m_tileSize.height() / 2.0, m_contentsRect);
            m_filterKeep->unite(keep);
        }
    }

    // Drop tiles outside the new keepRect.
    m_tiles.removeIf([&](auto& iter) {
        auto& tile = iter.value;
        if (m_filterKeep ? !m_filterKeep->intersects(tile.rect) : !tile.rect.intersects(m_keepRect)) {
            tilesToRemove.append(tile.id);
            return true;
        }
        return false;
    });

    if (m_filterCover ? m_filterCover->isEmpty() : m_coverRect.isEmpty())
        return;

    // Resize tiles at the edge in case the contents size has changed, but only do so
    // after having dropped tiles outside the keep rect.
    if (contentsRectChanged) {
        m_tiles.removeIf([&](auto& iter) {
            auto& tile = iter.value;
            auto expectedTileRect = tileRectForPosition(iter.key);
            if (expectedTileRect.isEmpty()) {
                tilesToRemove.append(tile.id);
                return true;
            }

            if (expectedTileRect != tile.rect)
                tile.resize(expectedTileRect.size());
            return false;
        });
    }

    // Search for the tile position closest to the viewport center that does not yet contain a tile.
    // Which position is considered the closest depends on the tileDistance function.
    double shortestDistance = std::numeric_limits<double>::infinity();
    IntPoint visibleCenterPosition = tilePositionForPoint(m_visibleRect.center());
    auto tileDistance = [&] (const IntPoint& tilePosition) -> double {
        if (m_filterInputCoverage ? m_filterInputCoverage->intersects(tileRectForPosition(tilePosition)) : m_visibleRect.intersects(tileRectForPosition(tilePosition)))
            return 0;

        return std::max(std::abs(visibleCenterPosition.y() - tilePosition.y()), std::abs(visibleCenterPosition.x() - tilePosition.x()));
    };

    // Cover areas (in tiles) with minimum distance from the visible rect. If the visible rect is
    // not covered already it will be covered first in one go, due to the distance being 0 for tiles
    // inside the visible rect.
    Vector<IntPoint> tilePositionsToCreate;
    unsigned requiredTileCount = 0;
    HashSet<IntPoint> visited;
    Function<void(IntPoint&&)> considerPosition = [&](IntPoint&& position) {
        if ((m_filterCover && !visited.add(position).isNewEntry) || m_tiles.contains(position))
            return;

        ++requiredTileCount;
        double distance = tileDistance(position);
        if (distance > shortestDistance)
            return;

        if (distance < shortestDistance) {
            tilePositionsToCreate.clear();
            shortestDistance = distance;
        }
        tilePositionsToCreate.append(position);
    };
    if (m_filterCover) {
        for (auto rect : m_filterCover->rects())
            forEachTilePositionInRect(rect, considerPosition);
    } else
        forEachTilePositionInRect(m_coverRect, considerPosition);

    if (requiredTileCount) {
        requiredTileCount -= tilePositionsToCreate.size();

        for (const auto& position : tilePositionsToCreate) {
            auto tile = Tile(generateTileID(), tileRectForPosition(position));
#if ENABLE(DAMAGE_TRACKING)
            IntRect unscaledDirtyRect = tile.dirtyRect;
            unscaledDirtyRect.scale(1 / contentsScale);
            damage.add(unscaledDirtyRect);
#else
            UNUSED_PARAM(damage);
#endif
            tilesToCreate.append(tile.id);
            m_tiles.add(position, WTF::move(tile));
        }
    }

    // Re-call createTiles on a timer to cover the visible area with the newest shortest distance.
    m_pendingTileCreation = requiredTileCount;
}

// This is based on the same heuristics used by chromium to compute the tile size.
IntSize CoordinatedBackingStoreProxy::computeTileSize(const IntSize& viewportSize, int maxTextureSize) const
{
    static IntSize overridenTileSize;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        const char* tileSizeEnv = getenv("WEBKIT_LAYERS_TILE_SIZE");
        if (tileSizeEnv && *tileSizeEnv) {
            auto tokens = String::fromLatin1(tileSizeEnv).split('x');
            if (!tokens.isEmpty()) {
                int width = parseInteger<int>(tokens[0]).value_or(0);
                int height = tokens.size() > 1 ? parseInteger<int>(tokens[1]).value_or(0) : width;
                overridenTileSize = { width, height };
            }

            if (overridenTileSize.isEmpty())
                WTFLogAlways("Invalid value '%s' for WEBKIT_LAYERS_TILE_SIZE, ignoring", tileSizeEnv);
        }
    });
    if (!overridenTileSize.isEmpty())
        return overridenTileSize;

    IntSize tileSize;
#if USE(SKIA)
#if PLATFORM(SCARLET)
    if (PlatformDisplay::sharedDisplay().prepareSkiaContext()) {
#else
    if (ProcessCapabilities::canUseAcceleratedBuffers() && PlatformDisplay::sharedDisplay().skiaGLContext()) {
#endif
#else
    if (ProcessCapabilities::canUseAcceleratedBuffers()) {
#endif
        static constexpr int minGPUTileHeight = 256;

        auto gpuTileSize = [&](const IntSize& baseSize) -> IntSize {
            int width = baseSize.width();
            int divisor = 4;
            if (m_contentsRect.width() <= baseSize.width() / 2)
                divisor = 2;
            if (m_contentsRect.width() <= baseSize.width() / 4)
                divisor = 1;
            int height = roundUpToMultipleOfNonPowerOfTwo(divisor, baseSize.height()) / divisor;
            return { width, std::max(height, minGPUTileHeight) };
        };

        IntSize baseSize = viewportSize;
        tileSize = gpuTileSize(baseSize);

        // Use half-width tiles when the contents width is greater than computed tile size.
        if (m_contentsRect.width() > tileSize.width()) {
            baseSize.setWidth((baseSize.width() + 1) / 2);
            tileSize = gpuTileSize(baseSize);
        }
    } else {
        tileSize = { s_defaultCPUTileSize, s_defaultCPUTileSize };

        static constexpr int maxUntiledContentSize = 512;
        // If the contents width is small, increase tile size vertically.
        if (m_contentsRect.width() < tileSize.width())
            tileSize.setHeight(maxUntiledContentSize);
        // If the contents height is small, increase tile size horizontally.
        if (m_contentsRect.height() < tileSize.height())
            tileSize.setWidth(maxUntiledContentSize);
        // If both are less than the untiled content size, use a single tile.
        if (m_contentsRect.width() < maxUntiledContentSize && m_contentsRect.height() < maxUntiledContentSize)
            tileSize = { maxUntiledContentSize, maxUntiledContentSize };
    }

    tileSize.clampToMaximumSize(m_contentsRect.size());
    if (maxTextureSize)
        tileSize.clampToMaximumSize({ maxTextureSize, maxTextureSize });

    return tileSize;
}

void CoordinatedBackingStoreProxy::adjustForContentsRect(IntRect& rect) const
{
    IntRect bounds = m_contentsRect;
    IntSize candidateSize = rect.size();

    rect.intersect(bounds);

    if (rect.size() == candidateSize)
        return;

    /*
     * In the following case, there is no intersection of the contents rect and the cover rect.
     * Thus the latter should not be inflated.
     *
     *  +----------------+
     *  | m_contentsRect |
     *  +----------------+
     *
     *          +-------------------------------+
     *          |          cover rect           |
     *          |         +---------+           |
     *          |         | visible |           |
     *          |         |  rect   |           |
     *          |         +---------+           |
     *          +-------------------------------+
     */
    if (rect.isEmpty())
        return;

    // Try to create a cover rect of the same size as the candidate, but within content bounds.
    int pixelsCovered = 0;
    if (!WTF::safeMultiply(candidateSize.width(), candidateSize.height(), pixelsCovered))
        pixelsCovered = std::numeric_limits<int>::max();

    if (rect.width() < candidateSize.width())
        rect.inflateY(((pixelsCovered / rect.width()) - rect.height()) / 2);
    if (rect.height() < candidateSize.height())
        rect.inflateX(((pixelsCovered / rect.height()) - rect.width()) / 2);

    rect.intersect(bounds);
}

std::pair<IntRect, IntRect> CoordinatedBackingStoreProxy::computeCoverAndKeepRect() const
{
    IntRect coverRect = m_visibleRect;
    IntRect keepRect = m_visibleRect;

    // If we cover more that the actual viewport we can be smart about which tiles we choose to render.
    if (m_coverAreaMultiplier > 1) {
        // The initial cover area covers equally in each direction, according to the coverAreaMultiplier.
        coverRect.inflateX(m_visibleRect.width() * (m_coverAreaMultiplier - 1) / 2);
        coverRect.inflateY(m_visibleRect.height() * (m_coverAreaMultiplier - 1) / 2);
        keepRect = coverRect;
        ASSERT(keepRect.contains(coverRect));
    }

    adjustForContentsRect(coverRect);

    // The keep rect is an inflated version of the cover rect, inflated in tile dimensions.
    keepRect.unite(coverRect);
    keepRect.inflateX(m_tileSize.width() / 2);
    keepRect.inflateY(m_tileSize.height() / 2);
    keepRect.intersect(m_contentsRect);

    ASSERT(coverRect.isEmpty() || keepRect.contains(coverRect));
    return { WTF::move(coverRect), WTF::move(keepRect) };
}

CoordinatedBackingStoreProxy::Update CoordinatedBackingStoreProxy::takePendingUpdate()
{
    Locker locker { m_update.lock };
    return WTF::move(m_update.pending);
}

IntRect CoordinatedBackingStoreProxy::mapToContents(const IntRect& rect) const
{
    return enclosingIntRect(FloatRect(rect.x() / m_contentsScale,
        rect.y() / m_contentsScale,
        rect.width() / m_contentsScale,
        rect.height() / m_contentsScale));
}

IntRect CoordinatedBackingStoreProxy::mapFromContents(const IntRect& rect) const
{
    return enclosingIntRect(FloatRect(rect.x() * m_contentsScale,
        rect.y() * m_contentsScale,
        rect.width() * m_contentsScale,
        rect.height() * m_contentsScale));
}

IntRect CoordinatedBackingStoreProxy::tileRectForPosition(const IntPoint& position) const
{
    IntRect rect(position.x() * m_tileSize.width(),
        position.y() * m_tileSize.height(),
        m_tileSize.width(),
        m_tileSize.height());

    rect.intersect(m_contentsRect);
    return rect;
}

IntPoint CoordinatedBackingStoreProxy::tilePositionForPoint(const IntPoint& point) const
{
    int x = point.x() / m_tileSize.width();
    int y = point.y() / m_tileSize.height();
    return IntPoint(std::max(x, 0), std::max(y, 0));
}

void CoordinatedBackingStoreProxy::forEachTilePositionInRect(const IntRect& rect, NOESCAPE const Function<void(IntPoint&&)>& callback)
{
    auto topLeft = tilePositionForPoint(rect.minXMinYCorner());
    auto innerBottomRight = tilePositionForPoint(rect.maxXMaxYCorner() - IntSize(1, 1));
    for (int y = topLeft.y(); y <= innerBottomRight.y(); ++y) {
        for (int x = topLeft.x(); x <= innerBottomRight.x(); ++x)
            callback(IntPoint(x, y));
    }
}

void CoordinatedBackingStoreProxy::waitUntilPaintingComplete()
{
    Locker locker { m_update.lock };
    m_update.pending.waitUntilPaintingComplete();
}

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS)
