/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#if USE(SCARLET_MEDIA) && ENABLE(MEDIA_SOURCE)
#include "SourceBufferPrivate.h"
#include "FloatSize.h"
#include "FloatSizeHash.h"
#include "ScarletMedia.h"
#include "SourceBufferPrivateClient.h"
#include <wtf/HashMap.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadSafeWeakPtr.h>
namespace WebCore {
class MediaSampleScarlet;
class MediaPlayerPrivateScarlet;
class MediaSourcePrivateScarlet;
class SourceBufferPrivateScarlet final : public SourceBufferPrivate {
public:
    static RefPtr<SourceBufferPrivateScarlet> create(MediaSourcePrivateScarlet&, const ContentType&);
    ~SourceBufferPrivateScarlet();
    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::Scarlet; }
    void removedFromMediaSource() final;
    void notifyDecodersReady();
    void endOfStream();
#if !RELEASE_LOG_DISABLED
    const Logger& sourceBufferLogger() const final;
    uint64_t sourceBufferLogIdentifier() final { return reinterpret_cast<uintptr_t>(this); }
#endif
protected:
    Ref<MediaPromise> appendInternal(Ref<SharedBuffer>&&) final;
    void resetParserStateInternal() final;
    void flush(TrackID) final;
    void enqueueSample(Ref<MediaSample>&&, TrackID) final;
    void allSamplesInTrackEnqueued(TrackID) final;
    bool isReadyForMoreSamples(TrackID) final;
    void notifyClientWhenReadyForMoreSamples(TrackID) final;
    size_t platformMaximumBufferSize() const final { return 128 * 1024 * 1024; }
    size_t platformEvictionThreshold() const final { return 96 * 1024 * 1024; }
private:
    SourceBufferPrivateScarlet(MediaSourcePrivateScarlet&, ScarletMediaParser*);
    RefPtr<MediaPlayerPrivateScarlet> player() const;
    void poll();
    void submitInput();
    void receiveTrack(const ScarletMediaTrack&);
    void deliverInitializationSegment();
    void finishAppend(bool);

    ThreadSafeWeakPtr<MediaSourcePrivateScarlet> m_parent;
    ScarletMediaParser* m_parser;
    RunLoop::Timer m_timer;
    HashMap<uint64_t, TrackID> m_trackIdentifiers;
    HashMap<TrackID, FloatSize> m_trackSizes;
    HashSet<TrackID> m_waitingTracks;
    HashSet<TrackID> m_fullyEnqueuedTracks;
    bool m_endOfStream { false };
    RefPtr<SharedBuffer> m_appendData;
    size_t m_appendOffset { 0 };
    uint64_t m_appendIdentifier { 0 };
    unsigned m_outstandingAppends { 0 };
    std::optional<MediaPromise::AutoRejectProducer> m_appendPromise;
    std::optional<InitializationSegment> m_initialization;
    RefPtr<MediaSampleScarlet> m_deferredSample;
};
}
#endif
