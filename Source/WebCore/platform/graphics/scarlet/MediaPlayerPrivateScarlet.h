/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#if USE(SCARLET_MEDIA)
#include "MediaPlayerPrivate.h"
#include "ColorSpace.h"
#include "VideoFrameMetadata.h"
#include "MediaSampleScarlet.h"
#include "PlatformMediaResourceLoader.h"
#include "ScarletMedia.h"
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/MonotonicTime.h>
namespace WebCore {
class MediaSourcePrivateScarlet;
class ScarletMediaResourceClient;
class MediaPlayerPrivateScarlet final
    : public MediaPlayerPrivateInterface
    , public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<MediaPlayerPrivateScarlet> {
public:
    static void registerMediaEngine(MediaEngineRegistrar);
    static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);
    explicit MediaPlayerPrivateScarlet(MediaPlayer&);
    ~MediaPlayerPrivateScarlet();
    void ref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref(); }
    void deref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref(); }
    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::Scarlet; }
    void load(const URL&, const LoadOptions&) final;
#if ENABLE(MEDIA_SOURCE)
    void load(const URL&, const LoadOptions&, MediaSourcePrivateClient&) final;
    void readyStateFromMediaSourceChanged() final;
    void characteristicsFromMediaSourceChanged() final;
    void seekableRangesFromMediaSourceChanged() final;
    void mediaSourceDurationChanged();
    void mediaSourceEndOfStream(bool networkError, bool decodeError);
#endif
    void cancelLoad() final;
    void play() final;
    void pause() final;
    FloatSize naturalSize() const final { return m_size; }
    bool hasVideo() const final { return m_status.has_video; }
    bool hasAudio() const final { return m_status.has_audio; }
    void setPageIsVisible(bool) final;
#if USE(COORDINATED_GRAPHICS)
    bool supportsAcceleratedRendering() const final;
    void acceleratedRenderingStateChanged() final;
    void setPlatformLayerBufferProxy(Ref<CoordinatedPlatformLayerBufferProxy>&&) final;
    RefPtr<CoordinatedPlatformLayerBufferProxy> platformLayerBufferProxy() const final;
#endif
    MediaTime duration() const final;
    MediaTime currentTime() const final;
    Ref<MediaTimePromise> seekToTarget(const SeekTarget&) final;
    void setRateDouble(double) final;
    double rate() const final { return m_rate; }
    bool paused() const final { return statusSnapshot().paused; }
    void setVolumeDouble(double) final;
    void setMuted(bool) final;
    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final;
    const PlatformTimeRanges& buffered() const final;
    const PlatformTimeRanges& seekable() const final;
    bool didLoadingProgress() const final { return std::exchange(m_loadingProgress, false); }
    void paint(GraphicsContext&, const FloatRect&) final;
    RefPtr<NativeImage> nativeImageForCurrentTime() final;
    ColorSpace colorSpace() final { return ColorSpace::SRGB(); }
    bool hasAvailableVideoFrame() const final {
#if USE(COORDINATED_GRAPHICS)
        return m_image || m_framePump;
#else
        return !!m_image;
#endif
    }
    bool didPassCORSAccessCheck() const final { return m_passedCors; }
    bool ended() const final { return m_status.ended; }
    unsigned decodedFrameCount() const final;
    unsigned droppedFrameCount() const final;
    std::optional<VideoPlaybackQualityMetrics> videoPlaybackQualityMetrics() final;
    std::optional<VideoFrameMetadata> videoFrameMetadata() final;
    void startVideoFrameMetadataGathering() final { m_gatheringVideoFrameMetadata = true; }
    void stopVideoFrameMetadataGathering() final { m_gatheringVideoFrameMetadata = false; m_videoFrameMetadata.reset(); }
    String engineDescription() const final { return "Scarlet native media / Skia Vulkan"_s; }

    bool configureTrack(const ScarletMediaTrack&);
    bool enqueueSample(const MediaSampleScarlet&);
    bool hasCapacity(TrackID) const;
    void flushTrack(TrackID);
    void removeTrack(TrackID);
    void endTrack(TrackID);
    void wake();
private:
    friend class ScarletMediaResourceClient;
    void receiveResponse(const ResourceResponse&);
    void receiveData(const SharedBuffer&);
    void finishLoading();
    void fail(MediaPlayer::NetworkState);
    void poll();
    void pumpInput();
    void pumpParser();
    void updateStatus();
    void updateFrame();
    void setNetworkState(MediaPlayer::NetworkState);
    void releaseParserEvent();
    void updateBuffered();
    ScarletMediaStatus statusSnapshot() const;
    void didPresentVideoFrame(uint64_t, double, const FloatSize&, MonotonicTime);
#if USE(COORDINATED_GRAPHICS)
    struct VideoFramePump;
    void stopFramePump();
    std::unique_ptr<VideoFramePump> m_framePump;
    uint64_t m_snapshotVideoFrameSerial { 0 };
    uint64_t m_pumpPresentedCount { 0 };
    bool pushFrameToCompositor();
    void clearCompositorFrame();
#endif

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    RunLoop::Timer m_timer;
    ScarletMediaSession* m_session { nullptr };
    ScarletMediaParser* m_parser { nullptr };
    RefPtr<PlatformMediaResource> m_resource;
    RefPtr<NativeImage> m_image;
#if USE(COORDINATED_GRAPHICS)
    RefPtr<CoordinatedPlatformLayerBufferProxy> m_contentsBufferProxy;
    uint64_t m_pushedVideoFrameSerial { 0 };
    uint64_t m_videoPresentationGeneration { 0 };
    uint64_t m_compositorProxyGeneration { 0 };
#endif
    uint64_t m_videoFrameSerial { 0 };
    uint64_t m_paintedVideoFrameSerial { 0 };
    uint64_t m_presentedVideoFrames { 0 };
    double m_frameMediaTime { 0 };
    std::optional<VideoFrameMetadata> m_videoFrameMetadata;
    bool m_gatheringVideoFrameMetadata { false };
    bool m_notifiedFirstVideoFrame { false };
#if ENABLE(MEDIA_SOURCE)
    RefPtr<MediaSourcePrivateScarlet> m_mediaSource;
#endif
    struct TrackState {
        uint32_t kind { 0 };
        PlatformTimeRanges ranges;
    };
    HashMap<TrackID, TrackState> m_tracks;
    Vector<Ref<MediaSampleScarlet>> m_packets;
    Deque<Ref<MediaSampleScarlet>> m_replay;
    size_t m_packetBytes { 0 };
    Deque<Ref<SharedBuffer>> m_input;
    size_t m_inputBytes { 0 };
    uint64_t m_appendIdentifier { 0 };
    uint64_t m_seekGeneration { 0 };
    std::optional<ScarletMediaParserEvent> m_parserEvent;
    std::optional<MediaTimePromise::AutoRejectProducer> m_seekPromise;
    MediaTime m_seekTarget { MediaTime::invalidTime() };
    ScarletMediaStatus m_status { };
    FloatSize m_size;
    mutable PlatformTimeRanges m_buffered;
    mutable PlatformTimeRanges m_seekable;
    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    double m_rate { 1 };
    bool m_visible { true };
    bool m_passedCors { false };
    bool m_inputFinished { false };
    bool m_endSubmitted { false };
    bool m_demuxEnded { false };
    bool m_replayPending { false };
    bool m_seekReadyForCompletion { false };
    mutable bool m_loadingProgress { false };
};
}
#endif
