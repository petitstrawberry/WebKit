/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#if USE(SCARLET_MEDIA)
#include "MediaPlayerPrivate.h"
#include "ColorSpace.h"
#include "MediaSampleScarlet.h"
#include "PlatformMediaResourceLoader.h"
#include "ScarletMedia.h"
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadSafeWeakPtr.h>
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
    void setPageIsVisible(bool visible) final { m_visible = visible; }
    MediaTime duration() const final;
    MediaTime currentTime() const final;
    Ref<MediaTimePromise> seekToTarget(const SeekTarget&) final;
    void setRateDouble(double) final;
    double rate() const final { return m_rate; }
    bool paused() const final { return m_status.paused; }
    void setVolumeDouble(double) final;
    void setMuted(bool) final;
    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final;
    const PlatformTimeRanges& buffered() const final;
    const PlatformTimeRanges& seekable() const final;
    bool didLoadingProgress() const final { return std::exchange(m_loadingProgress, false); }
    void paint(GraphicsContext&, const FloatRect&) final;
    RefPtr<NativeImage> nativeImageForCurrentTime() final { return m_image; }
    ColorSpace colorSpace() final { return ColorSpace::SRGB(); }
    bool hasAvailableVideoFrame() const final { return !!m_image; }
    bool didPassCORSAccessCheck() const final { return m_passedCors; }
    bool ended() const final { return m_status.ended; }
    unsigned decodedFrameCount() const final { return m_status.decoded_frames; }
    unsigned droppedFrameCount() const final { return m_status.dropped_frames; }
    String engineDescription() const final { return "Scarlet native media / Skia Vulkan"_s; }

    bool configureTrack(const ScarletMediaTrack&);
    bool enqueueSample(const MediaSampleScarlet&);
    bool hasCapacity(TrackID) const;
    void flushTrack(TrackID);
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

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    RunLoop::Timer m_timer;
    ScarletMediaSession* m_session { nullptr };
    ScarletMediaParser* m_parser { nullptr };
    RefPtr<PlatformMediaResource> m_resource;
    RefPtr<NativeImage> m_image;
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
    std::optional<ScarletMediaParserEvent> m_parserEvent;
    std::optional<MediaTimePromise::AutoRejectProducer> m_seekPromise;
    MediaTime m_seekTarget { MediaTime::invalidTime() };
    ScarletMediaStatus m_status { };
    FloatSize m_size;
    mutable PlatformTimeRanges m_buffered;
    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    double m_rate { 1 };
    bool m_visible { true };
    bool m_passedCors { false };
    bool m_inputFinished { false };
    bool m_endSubmitted { false };
    bool m_seekReadyForCompletion { false };
    mutable bool m_loadingProgress { false };
};
}
#endif
