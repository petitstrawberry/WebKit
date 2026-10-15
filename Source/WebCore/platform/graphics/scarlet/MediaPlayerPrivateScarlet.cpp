/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "config.h"
#include "MediaPlayerPrivateScarlet.h"
#if USE(SCARLET_MEDIA)
#include "ContentType.h"
#include "GraphicsContext.h"
#include "MediaPlayer.h"
#include "PlatformDisplay.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SkiaGpuContext.h"
#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedPlatformLayerBufferProxy.h"
#include "CoordinatedPlatformLayerBufferSkiaImage.h"
#endif
#if ENABLE(MEDIA_SOURCE)
#include "MediaSourcePrivateScarlet.h"
#endif
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkPixmap.h>
#include <skia/core/SkYUVAPixmaps.h>
#include <skia/gpu/ganesh/SkImageGanesh.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <wtf/Scope.h>
#include <wtf/TZoneMallocInlines.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include "SkiaUtilities.h"
#include <wtf/threads/BinarySemaphore.h>
#include <wtf/MonotonicTime.h>
#include <wtf/text/CString.h>

namespace WebCore {
// Lifecycle points only: no frame-rate logging, URLs, source bytes, or tokens.
static void profileMediaLifecycle(const char* stage, ScarletMediaSession* session,
    MediaPlayer::NetworkState networkState, size_t stagedBytes, bool mediaSource,
    bool inputFinished, bool endSubmitted, bool demuxEnded, int detail = 0)
{
    static const bool enabled = [] {
        auto* value = std::getenv("SCARLET_WEBVIEW_PROFILE");
        return value && value[0] == '1' && !value[1];
    }();
    if (!enabled)
        return;
    ScarletMediaStatus status { };
    if (session)
        scarlet_media_session_status(session, &status);
    // Scarlet's standard printf family accepts integer/string conversions.
    // Convert the actual doubles through WTF instead of truncating the log at
    // an unsupported floating-point printf conversion.
    auto time = String::number(status.current_time).utf8();
    auto duration = String::number(status.duration).utf8();
    std::fprintf(stderr, "SCARLET_MEDIA_LIFECYCLE stage=%s session=%p network_state=%u detail=%d staged_bytes=%zu mse=%u input_eof=%u end_submitted=%u demux_ended=%u native_ready=%u native_paused=%u native_ended=%u native_error=%d time=%s duration=%s decoded=%llu\n",
        stage, static_cast<void*>(session), static_cast<unsigned>(networkState), detail,
        stagedBytes, static_cast<unsigned>(mediaSource), static_cast<unsigned>(inputFinished),
        static_cast<unsigned>(endSubmitted), static_cast<unsigned>(demuxEnded),
        status.ready_state, status.paused, status.ended, status.error,
        reinterpret_cast<const char*>(time.data()), reinterpret_cast<const char*>(duration.data()), static_cast<unsigned long long>(status.decoded_frames));
}

// Aggregate diagnostics for polls on this owner. No player lifetime state,
// public engine layout changes, or clock reads when profiling is disabled.
enum class MediaPollPhase : unsigned {
    Poll, Input, Parser, Status, Acquire, Upload, Replace, Push, Release, Notify, Count
};
struct MediaPollProfile {
    struct Counter { uint64_t calls { 0 }, micros { 0 }, maximum { 0 }; };
    Counter counters[static_cast<unsigned>(MediaPollPhase::Count)];
    MonotonicTime since { MonotonicTime::now() };
};
static bool mediaPollProfilingEnabled()
{
    static const bool enabled = [] {
        auto* value = std::getenv("SCARLET_WEBVIEW_PROFILE");
        return value && value[0] == '1' && !value[1];
    }();
    return enabled;
}
static MediaPollProfile* mediaPollProfile()
{
    if (!mediaPollProfilingEnabled())
        return nullptr;
    static thread_local MediaPollProfile profile;
    return &profile;
}
static void addMediaProfileCounter(uint64_t& counter, uint64_t amount)
{
    counter += std::min(amount, std::numeric_limits<uint64_t>::max() - counter);
}
class MediaPollPhaseScope {
public:
    MediaPollPhaseScope(MediaPollProfile* profile, MediaPollPhase phase)
        : m_profile(profile), m_phase(phase)
    {
        if (m_profile)
            m_start = MonotonicTime::now();
    }
    ~MediaPollPhaseScope()
    {
        if (!m_profile)
            return;
        auto elapsed = (MonotonicTime::now() - m_start).microseconds();
        auto micros = elapsed >= static_cast<double>(std::numeric_limits<uint64_t>::max()) ? std::numeric_limits<uint64_t>::max() : elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
        auto& counter = m_profile->counters[static_cast<unsigned>(m_phase)];
        addMediaProfileCounter(counter.calls, 1);
        addMediaProfileCounter(counter.micros, micros);
        counter.maximum = std::max(counter.maximum, micros);
    }
private:
    MediaPollProfile* m_profile;
    MediaPollPhase m_phase;
    MonotonicTime m_start;
};
template<typename Function>
static decltype(auto) profileMediaPollPhase(MediaPollProfile* profile, MediaPollPhase phase, Function&& function)
{
    MediaPollPhaseScope scope(profile, phase);
    return function();
}
static void reportMediaPollProfile(MediaPollProfile& profile)
{
    auto now = MonotonicTime::now();
    auto elapsed = (now - profile.since).microseconds();
    if (elapsed < 1000000)
        return;
    auto value = [&](MediaPollPhase phase) { return static_cast<unsigned long long>(profile.counters[static_cast<unsigned>(phase)].micros); };
    const auto& total = profile.counters[static_cast<unsigned>(MediaPollPhase::Poll)];
    const auto& acquire = profile.counters[static_cast<unsigned>(MediaPollPhase::Acquire)];
    const auto& upload = profile.counters[static_cast<unsigned>(MediaPollPhase::Upload)];
    std::fprintf(stderr, "SCARLET_MEDIA_POLL_PROFILE scope=owner-polls period_us=%llu polls=%llu poll_us=%llu max_poll_us=%llu input_us=%llu parser_us=%llu status_us=%llu acquire_calls=%llu acquire_us=%llu upload_calls=%llu upload_us=%llu replace_us=%llu push_us=%llu release_us=%llu notify_us=%llu\n",
        static_cast<unsigned long long>(elapsed),
        static_cast<unsigned long long>(total.calls), static_cast<unsigned long long>(total.micros), static_cast<unsigned long long>(total.maximum),
        value(MediaPollPhase::Input), value(MediaPollPhase::Parser), value(MediaPollPhase::Status),
        static_cast<unsigned long long>(acquire.calls), value(MediaPollPhase::Acquire),
        static_cast<unsigned long long>(upload.calls), value(MediaPollPhase::Upload),
        value(MediaPollPhase::Replace), value(MediaPollPhase::Push), value(MediaPollPhase::Release), value(MediaPollPhase::Notify));
    profile = { };
    profile.since = now;
}

class MediaPlayerFactoryScarlet final : public MediaPlayerFactory {
    WTF_MAKE_TZONE_ALLOCATED(MediaPlayerFactoryScarlet);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(MediaPlayerFactoryScarlet);
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::Scarlet; }
    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final { return adoptRef(*new MediaPlayerPrivateScarlet(player)); }
    void getSupportedTypes(HashSet<String>& types) const final
    {
        for (auto mime : { "video/mp4"_s, "audio/mp4"_s }) {
            auto bytes = String { mime }.utf8();
            if (scarlet_media_supports_type(reinterpret_cast<const char*>(bytes.data()), bytes.length(), nullptr, 0, 0) > 0)
                types.add(mime);
        }
    }
    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final { return MediaPlayerPrivateScarlet::supportsType(parameters); }
};
WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerFactoryScarlet);

void MediaPlayerPrivateScarlet::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<MediaPlayerFactoryScarlet>());
}

MediaPlayer::SupportsType MediaPlayerPrivateScarlet::supportsType(const MediaEngineSupportParameters& parameters)
{
    if (parameters.requiresRemotePlayback)
        return MediaPlayer::SupportsType::IsNotSupported;
    auto mime = parameters.type.containerType().utf8();
    auto codecs = parameters.type.parameter(ContentType::codecsParameter()).utf8();
    bool isMSE = parameters.platformType == PlatformMediaDecodingType::MediaSource;
    if (mime.isNull() || !mime.length())
        return isMSE ? MediaPlayer::SupportsType::IsNotSupported : MediaPlayer::SupportsType::MayBeSupported;
    if (scarlet_media_supports_type(reinterpret_cast<const char*>(mime.data()), mime.length(), reinterpret_cast<const char*>(codecs.data()), codecs.length(), isMSE) <= 0)
        return MediaPlayer::SupportsType::IsNotSupported;
    return codecs.length() ? MediaPlayer::SupportsType::IsSupported : MediaPlayer::SupportsType::MayBeSupported;
}

class ScarletMediaResourceClient final : public PlatformMediaResourceClient {
public:
    explicit ScarletMediaResourceClient(MediaPlayerPrivateScarlet& player) : m_player(player) { }
    void responseReceived(PlatformMediaResource& resource, const ResourceResponse& response, CompletionHandler<void(ShouldContinuePolicyCheck)>&& completion) final
    {
        if (auto player = m_player.get(); player && player->m_resource.get() == &resource) {
            player->receiveResponse(response);
            completion(player->networkState() == MediaPlayer::NetworkState::Loading ? ShouldContinuePolicyCheck::Yes : ShouldContinuePolicyCheck::No);
        } else
            completion(ShouldContinuePolicyCheck::No);
    }
    void dataReceived(PlatformMediaResource& resource, const SharedBuffer& data) final
    {
        if (auto player = m_player.get(); player && player->m_resource.get() == &resource)
            player->receiveData(data);
    }
    void loadFinished(PlatformMediaResource& resource, const NetworkLoadMetrics&) final
    {
        if (auto player = m_player.get(); player && player->m_resource.get() == &resource)
            player->finishLoading();
    }
    void loadFailed(PlatformMediaResource& resource, const ResourceError&) final
    {
        if (auto player = m_player.get(); player && player->m_resource.get() == &resource)
            player->fail(MediaPlayer::NetworkState::NetworkError);
    }
    void accessControlCheckFailed(PlatformMediaResource& resource, const ResourceError&) final
    {
        if (auto player = m_player.get(); player && player->m_resource.get() == &resource)
            player->fail(MediaPlayer::NetworkState::NetworkError);
    }
private:
    ThreadSafeWeakPtr<MediaPlayerPrivateScarlet> m_player;
};

void MediaPlayerPrivateScarlet::load(const URL& url, const LoadOptions&)
{
    cancelLoad();
    m_session = scarlet_media_session_create();
    if (!m_session) {
        fail(MediaPlayer::NetworkState::DecodeError);
        return;
    }
    RefPtr player = m_player.get();
    if (!player) {
        cancelLoad();
        return;
    }
    m_rate = player->requestedRate();
    scarlet_media_session_set_rate(m_session, m_rate);
    scarlet_media_session_set_volume(m_session, player->volume());
    scarlet_media_session_set_muted(m_session, player->muted());
    setNetworkState(MediaPlayer::NetworkState::Loading);
    m_resource = player->mediaResourceLoader()->requestResource(ResourceRequest { URL { url } }, PlatformMediaResourceLoader::DisallowCaching);
    if (!m_resource) {
        fail(MediaPlayer::NetworkState::NetworkError);
        return;
    }
    m_resource->setClient(adoptRef(*new ScarletMediaResourceClient(*this)));
    wake();
}

void MediaPlayerPrivateScarlet::receiveResponse(const ResourceResponse& response)
{
    if (response.httpStatusCode() >= 400) {
        fail(MediaPlayer::NetworkState::NetworkError);
        return;
    }
    auto mimeName = response.mimeType();
    if (mimeName.isEmpty() || mimeName == "application/octet-stream"_s)
        mimeName = "video/mp4"_s;
    auto mime = mimeName.utf8();
    if (scarlet_media_supports_type(reinterpret_cast<const char*>(mime.data()), mime.length(), nullptr, 0, 0) <= 0) {
        fail(MediaPlayer::NetworkState::FormatError);
        return;
    }
    m_parser = scarlet_media_parser_create(reinterpret_cast<const char*>(mime.data()), mime.length(), false);
    if (!m_parser)
        fail(MediaPlayer::NetworkState::FormatError);
}

void MediaPlayerPrivateScarlet::receiveData(const SharedBuffer& data)
{
    if (!m_parser || m_networkState != MediaPlayer::NetworkState::Loading)
        return;
    // PlatformMediaResource has no suspend API. Keep a bounded staging queue;
    // a resource that outruns the parser beyond this limit fails explicitly.
    constexpr size_t maximumStagedBytes = 16 * 1024 * 1024;
    if (data.size() > maximumStagedBytes - m_inputBytes) {
        fail(MediaPlayer::NetworkState::NetworkError);
        return;
    }
    auto bytes = data.span();
    for (size_t offset = 0; offset < bytes.size();) {
        auto size = std::min<size_t>(bytes.size() - offset, SCARLET_MEDIA_MAX_APPEND_BYTES);
        m_input.append(SharedBuffer::create(bytes.subspan(offset, size)));
        offset += size;
    }
    m_inputBytes += data.size();
    m_loadingProgress = true;
    pumpInput();
    wake();
}

void MediaPlayerPrivateScarlet::finishLoading()
{
    m_inputFinished = true;
    profileMediaLifecycle("resource-eof", m_session, m_networkState, m_inputBytes,
        false, m_inputFinished, m_endSubmitted, m_demuxEnded);
    if (m_resource)
        m_passedCors = m_resource->didPassAccessControlCheck();
    pumpInput();
    wake();
}

void MediaPlayerPrivateScarlet::releaseParserEvent()
{
    if (auto event = std::exchange(m_parserEvent, std::nullopt))
        scarlet_media_parser_release(m_parser, event->lease);
}

void MediaPlayerPrivateScarlet::cancelLoad()
{
#if USE(COORDINATED_GRAPHICS)
    stopFramePump(false);
#endif
    m_timer.stop();
    ++m_seekGeneration;
    m_seekPromise.reset();
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource) {
        m_mediaSource->cancelPendingWaitForTarget();
        m_mediaSource->setPlayer(nullptr);
    }
    m_mediaSource = nullptr;
#endif
    if (auto resource = std::exchange(m_resource, nullptr)) {
        resource->setClient(nullptr);
        resource->shutdown();
    }
    releaseParserEvent();
    if (auto parser = std::exchange(m_parser, nullptr))
        scarlet_media_parser_destroy(parser);
    if (auto session = std::exchange(m_session, nullptr))
        scarlet_media_session_destroy(session);
    m_input.clear();
    m_inputBytes = 0;
    m_packets.clear();
    m_replay.clear();
    m_packetBytes = 0;
    m_tracks.clear();
    m_buffered.clear();
    m_seekable.clear();
#if USE(COORDINATED_GRAPHICS)
    clearCompositorFrame();
#endif
    m_image = nullptr;
    m_size = { };
    m_videoFrameSerial = m_paintedVideoFrameSerial = m_presentedVideoFrames = 0;
    m_frameMediaTime = 0;
    m_videoFrameMetadata.reset();
    m_notifiedFirstVideoFrame = false;
#if USE(COORDINATED_GRAPHICS)
    m_contentsBufferProxy = nullptr;
#endif
    // Metadata gathering is a client request. MediaPlayer may start it before
    // invoking load(), so preserve that request across cancel/reload.
    m_status = { };
    m_status.paused = true;
    m_inputFinished = m_endSubmitted = m_demuxEnded = m_replayPending = m_passedCors = m_loadingProgress = false;
    m_seekTarget = MediaTime::invalidTime();
    m_seekReadyForCompletion = false;
    setNetworkState(MediaPlayer::NetworkState::Empty);
}

void MediaPlayerPrivateScarlet::setNetworkState(MediaPlayer::NetworkState state)
{
    if (m_networkState == state)
        return;
    m_networkState = state;
    if (auto player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateScarlet::fail(MediaPlayer::NetworkState state)
{
    bool mediaSource = false;
#if ENABLE(MEDIA_SOURCE)
    mediaSource = !!m_mediaSource;
#endif
    if (m_networkState != state)
        profileMediaLifecycle("fail", m_session, m_networkState, m_inputBytes,
            mediaSource, m_inputFinished, m_endSubmitted, m_demuxEnded, static_cast<int>(state));
    m_timer.stop();
    ++m_seekGeneration;
    m_seekTarget = MediaTime::invalidTime();
    m_seekReadyForCompletion = false;
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource)
        m_mediaSource->cancelPendingWaitForTarget();
#endif
    if (m_session)
        scarlet_media_session_pause(m_session);
    if (auto resource = std::exchange(m_resource, nullptr)) {
        resource->setClient(nullptr);
        resource->shutdown();
    }
    m_seekPromise.reset();
    setNetworkState(state);
}

void MediaPlayerPrivateScarlet::pumpInput()
{
    if (!m_parser)
        return;
    while (!m_input.isEmpty()) {
        auto& bytes = m_input.first();
        auto result = scarlet_media_parser_append(m_parser, bytes->span().data(), bytes->size(), m_appendIdentifier + 1);
        if (result == SCARLET_MEDIA_AGAIN)
            break;
        if (result != SCARLET_MEDIA_OK) {
            fail(MediaPlayer::NetworkState::DecodeError);
            return;
        }
        ++m_appendIdentifier;
        m_inputBytes -= bytes->size();
        m_input.removeFirst();
    }
    if (m_inputFinished && m_input.isEmpty() && !m_endSubmitted) {
        auto result = scarlet_media_parser_end(m_parser, m_appendIdentifier + 1);
        if (result == SCARLET_MEDIA_OK) {
            ++m_appendIdentifier;
            m_endSubmitted = true;
        } else if (result != SCARLET_MEDIA_AGAIN)
            fail(MediaPlayer::NetworkState::DecodeError);
    }
}

bool MediaPlayerPrivateScarlet::configureTrack(const ScarletMediaTrack& track)
{
    if (!m_session || scarlet_media_session_configure(m_session, &track) != SCARLET_MEDIA_OK)
        return false;
    m_tracks.ensure(track.identifier, [&] { return TrackState { track.kind, { } }; });
    if (track.kind == SCARLET_MEDIA_VIDEO) {
        auto size = FloatSize { static_cast<float>(track.width), static_cast<float>(track.height) };
        if (m_size != size) {
            m_size = size;
            if (auto player = m_player.get())
                player->sizeChanged();
        }
    }
    wake();
    return true;
}
bool MediaPlayerPrivateScarlet::enqueueSample(const MediaSampleScarlet& sample)
{
    if (!m_session)
        return false;
    auto packet = sample.bridgeSample();
    auto result = scarlet_media_session_enqueue(m_session, &packet);
    if (result < 0)
        fail(MediaPlayer::NetworkState::DecodeError);
    wake();
    return result == SCARLET_MEDIA_OK;
}
bool MediaPlayerPrivateScarlet::hasCapacity(TrackID track) const
{
    return m_session && scarlet_media_session_has_capacity(m_session, track);
}
void MediaPlayerPrivateScarlet::flushTrack(TrackID track)
{
#if USE(COORDINATED_GRAPHICS)
    clearCompositorFrame();
#endif
    if (m_session)
        scarlet_media_session_flush(m_session, track);
}
void MediaPlayerPrivateScarlet::removeTrack(TrackID identifier)
{
    auto track = m_tracks.find(identifier);
    bool removedVideo = track != m_tracks.end() && track->value.kind == SCARLET_MEDIA_VIDEO;
    m_tracks.remove(identifier);
    if (m_session)
        scarlet_media_session_remove_track(m_session, identifier);
    if (removedVideo) {
#if USE(COORDINATED_GRAPHICS)
        clearCompositorFrame();
#endif
        m_image = nullptr;
        m_videoFrameMetadata.reset();
        m_size = { };
        if (auto player = m_player.get()) {
            player->sizeChanged();
            player->repaint();
        }
    }
    wake();
}
void MediaPlayerPrivateScarlet::endTrack(TrackID track)
{
    if (m_session)
        scarlet_media_session_end(m_session, track);
    wake();
}

void MediaPlayerPrivateScarlet::pumpParser()
{
    while (!m_replay.isEmpty()) {
        if (!enqueueSample(m_replay.first()))
            return;
        m_replay.removeFirst();
    }
    if (std::exchange(m_replayPending, false) && m_demuxEnded) {
        for (auto& [identifier, track] : m_tracks)
            endTrack(identifier);
    }
    if (!m_parser)
        return;
    for (unsigned count = 0; count < 64; ++count) {
        if (!m_parserEvent) {
            ScarletMediaParserEvent event { };
            auto result = scarlet_media_parser_poll(m_parser, &event);
            if (!result)
                return;
            if (result < 0) {
                fail(MediaPlayer::NetworkState::DecodeError);
                return;
            }
            m_parserEvent = event;
        }
        auto& event = *m_parserEvent;
        if (event.kind == SCARLET_MEDIA_TRACK) {
            if (!configureTrack(event.track)) {
                fail(MediaPlayer::NetworkState::DecodeError);
                return;
            }
        } else if (event.kind == SCARLET_MEDIA_SAMPLE) {
            if (!hasCapacity(event.sample.track))
                return;
            auto packet = MediaSampleScarlet::create(event.sample, m_size);
            if (!enqueueSample(packet))
                return;
            constexpr size_t maximumSeekCache = 128 * 1024 * 1024;
            if (packet->sizeInBytes() > maximumSeekCache - m_packetBytes) {
                fail(MediaPlayer::NetworkState::DecodeError);
                return;
            }
            m_packetBytes += packet->sizeInBytes();
            auto track = m_tracks.find(packet->trackID());
            if (track != m_tracks.end())
                track->value.ranges.add(packet->presentationTime(), packet->presentationEndTime());
            m_packets.append(WTF::move(packet));
            updateBuffered();
        } else if (event.kind == SCARLET_MEDIA_PARSE_ERROR) {
            if (m_networkState != MediaPlayer::NetworkState::DecodeError)
                profileMediaLifecycle("parser-error", m_session, m_networkState, m_inputBytes,
                    false, m_inputFinished, m_endSubmitted, m_demuxEnded, event.error);
            fail(MediaPlayer::NetworkState::DecodeError);
            return;
        } else if (event.kind == SCARLET_MEDIA_APPEND_DONE && m_endSubmitted && event.append_identifier == m_appendIdentifier) {
            m_demuxEnded = true;
            profileMediaLifecycle("parser-eof", m_session, m_networkState, m_inputBytes,
                false, m_inputFinished, m_endSubmitted, m_demuxEnded);
            for (auto& [identifier, track] : m_tracks)
                endTrack(identifier);
            setNetworkState(MediaPlayer::NetworkState::Loaded);
        }
        releaseParserEvent();
    }
}

void MediaPlayerPrivateScarlet::updateBuffered()
{
    m_buffered.clear();
    bool first = true;
    for (auto& [identifier, track] : m_tracks) {
        if (first) {
            m_buffered = track.ranges;
            first = false;
        } else
            m_buffered.intersectWith(track.ranges);
    }
}

void MediaPlayerPrivateScarlet::wake()
{
    if (m_session && !m_timer.isActive())
        m_timer.startOneShot(0_s);
}
void MediaPlayerPrivateScarlet::poll()
{
    Ref protectedThis { *this };
    auto* profile = mediaPollProfile();
    {
        MediaPollPhaseScope pollScope(profile, MediaPollPhase::Poll);
        profileMediaPollPhase(profile, MediaPollPhase::Input, [&] { pumpInput(); });
        profileMediaPollPhase(profile, MediaPollPhase::Parser, [&] { pumpParser(); });
        profileMediaPollPhase(profile, MediaPollPhase::Status, [&] { updateStatus(); });
        updateFrame();
#if ENABLE(MEDIA_SOURCE)
        if (m_mediaSource)
            profileMediaPollPhase(profile, MediaPollPhase::Notify, [&] { m_mediaSource->notifyDecodersReady(); });
#endif
    }
    if (profile)
        reportMediaPollProfile(*profile);
    if (m_session && m_networkState != MediaPlayer::NetworkState::DecodeError && m_networkState != MediaPlayer::NetworkState::NetworkError
        && (!m_status.paused || m_networkState == MediaPlayer::NetworkState::Loading || m_seekPromise || !m_replay.isEmpty()))
        m_timer.startOneShot(Seconds { 1.0 / 60 });
}

void MediaPlayerPrivateScarlet::updateStatus()
{
    if (!m_session)
        return;
    ScarletMediaStatus status { };
    if (scarlet_media_session_status(m_session, &status) < 0 || status.error) {
        fail(MediaPlayer::NetworkState::DecodeError);
        return;
    }
    auto old = std::exchange(m_status, status);
    RefPtr player = m_player.get();
    if (!player)
        return;
    if (old.ready_state != status.ready_state)
        player->readyStateChanged();
    if (old.duration != status.duration)
        player->durationChanged();
    if (old.has_video != status.has_video || old.has_audio != status.has_audio)
        player->characteristicChanged();
    if (old.paused != status.paused)
        player->playbackStateChanged();
    if (old.ended != status.ended)
        player->timeChanged();
    if (m_seekPromise && m_seekReadyForCompletion && (status.ready_state >= static_cast<unsigned>(MediaPlayer::ReadyState::HaveCurrentData) || status.ended)) {
        auto seek = std::exchange(m_seekPromise, std::nullopt);
        auto target = std::exchange(m_seekTarget, MediaTime::invalidTime());
        seek->resolve(target);
        player->timeChanged();
    }
}

static SkYUVColorSpace videoColorSpace(const ScarletMediaFrame& frame)
{
    if (frame.matrix == SCARLET_MEDIA_BT709)
        return frame.full_range ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
    if (frame.matrix == SCARLET_MEDIA_BT2020)
        return frame.full_range ? kBT2020_8bit_Full_SkYUVColorSpace : kBT2020_8bit_Limited_SkYUVColorSpace;
    return frame.full_range ? kJPEG_Full_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
}
static sk_sp<SkImage> uploadVideoFrame(const ScarletMediaFrame& frame)
{
    if (!frame.width || !frame.height || frame.width > 16384 || frame.height > 16384
        || (frame.format != SCARLET_MEDIA_I420 && frame.format != SCARLET_MEDIA_NV12) || !prepareSkiaContext()) {
        
        return nullptr;
    }
    auto* context = PlatformDisplay::sharedDisplay().skiaGrContext();
    auto configuration = frame.format == SCARLET_MEDIA_I420 ? SkYUVAInfo::PlaneConfig::kY_U_V : SkYUVAInfo::PlaneConfig::kY_UV;
    SkYUVAInfo info { SkISize::Make(frame.width, frame.height), configuration, SkYUVAInfo::Subsampling::k420, videoColorSpace(frame) };
    SkPixmap planes[SkYUVAInfo::kMaxPlanes];
    unsigned count = frame.format == SCARLET_MEDIA_I420 ? 3 : 2;
    for (unsigned i = 0; i < count; ++i) {
        unsigned width = i ? (frame.width + 1) / 2 : frame.width;
        unsigned height = i ? (frame.height + 1) / 2 : frame.height;
        unsigned channels = i && frame.format == SCARLET_MEDIA_NV12 ? 2 : 1;
        if (!frame.planes[i] || frame.strides[i] < width * channels || frame.strides[i] > SIZE_MAX / height
            || frame.plane_sizes[i] < frame.strides[i] * height) {
            
            return nullptr;
        }
        auto imageInfo = SkImageInfo::Make(width, height, channels == 2 ? kR8G8_unorm_SkColorType : kGray_8_SkColorType, kOpaque_SkAlphaType);
        planes[i].reset(imageInfo, frame.planes[i], frame.strides[i]);
    }
    auto pixmaps = SkYUVAPixmaps::FromExternalPixmaps(info, planes);
    auto image = SkImages::TextureFromYUVAPixmaps(context, pixmaps, skgpu::Mipmapped::kNo, false, SkColorSpace::MakeSRGB());
    if (!image) {
        
        return nullptr;
    }
    return image;
}
#if USE(COORDINATED_GRAPHICS)
struct MediaPlayerPrivateScarlet::VideoFramePump {
    struct Snapshot {
        uint64_t serial { 0 }; double mediaTime { 0 }; FloatSize size; bool failed { false };
        uint64_t presentedCount { 0 }, presentedSerial { 0 };
        double presentedMediaTime { 0 };
        FloatSize presentedSize;
        MonotonicTime presentedAt;
    };
    struct Feedback { std::mutex mutex; Snapshot latest; };
    Ref<RunLoop> runLoop;
    RunLoop::Timer timer;
    ScarletMediaSession* session;
    Ref<CoordinatedPlatformLayerBufferProxy> proxy;
    uint64_t serial;
    std::shared_ptr<Feedback> feedback { std::make_shared<Feedback>() };
    sk_sp<SkImage> image; // Compositor only.
    VideoFramePump(RunLoop& loop, ScarletMediaSession* s, CoordinatedPlatformLayerBufferProxy& p,
        MediaPlayerPrivateScarlet& media)
        : runLoop(loop)
        , timer(loop, "ScarletVideoFramePump"_s, [this] { poll(); })
        , session(s), proxy(p), serial(media.m_videoFrameSerial)
    { }
    void poll()
    {
        RELEASE_ASSERT(!isMainThread());
        if (!proxy->isValid()) return;
        ScarletMediaStatus status { };
        scarlet_media_session_status(session, &status);
        timer.startOneShot(Seconds::fromMilliseconds(status.paused ? 100 : 8));
        ScarletMediaFrame frame { };
        if (scarlet_media_session_acquire_frame(session, &frame) <= 0) return;
        auto release = makeScopeExit([&] { scarlet_media_session_release_frame(session, frame.lease); });
        image = uploadVideoFrame(frame);
        if (!image) { std::lock_guard lock(feedback->mutex); feedback->latest.failed = true; return; }
        auto size = FloatSize { static_cast<float>(frame.width), static_cast<float>(frame.height) };
        double mediaTime = frame.presentation_us / 1000000.0;
        ++serial;
        auto buffer = makeUnique<CoordinatedPlatformLayerBufferSkiaImage>(sk_sp { image },
            CoordinatedPlatformLayerBuffer::AlphaMode::Opaque, CoordinatedPlatformLayerBuffer::Rotation::None);
        buffer->setPresentationCallback([feedback = feedback, serial = serial, mediaTime, size](MonotonicTime time) {
            // A bounded latest-value mailbox replaces per-frame owner dispatch.
            // No DOM/media object is retained or accessed by the compositor.
            std::lock_guard lock(feedback->mutex);
            ++feedback->latest.presentedCount;
            feedback->latest.presentedSerial = serial;
            feedback->latest.presentedMediaTime = mediaTime;
            feedback->latest.presentedSize = size;
            feedback->latest.presentedAt = time;
        });
        proxy->setDisplayBuffer(WTF::move(buffer));
        std::lock_guard lock(feedback->mutex);
        feedback->latest.serial = serial;
        feedback->latest.mediaTime = mediaTime;
        feedback->latest.size = size;
    }
    Snapshot snapshot() { std::lock_guard lock(feedback->mutex); return feedback->latest; }
    sk_sp<SkImage> snapshotForOwner(const sk_sp<GrContextThreadSafeProxy>& ownerProxy)
    {
        // Explicit canvas/video capture only. Normal video never makes this
        // RGB snapshot or waits for the Page owner.
        sk_sp<SkImage> result;
        BinarySemaphore done;
        runLoop->dispatch([&] {
            if (image) {
                auto* context = PlatformDisplay::sharedDisplay().skiaGrContext();
                auto info = SkImageInfo::Make(image->width(), image->height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
                if (auto surface = SkSurfaces::RenderTarget(context, skgpu::Budgeted::kYes, info)) {
                    surface->getCanvas()->drawImage(image, 0, 0);
                    result = SkiaUtilities::createPromiseImageIfNeeded(surface->makeImageSnapshot(), ownerProxy);
                }
            }
            done.signal();
        });
        done.wait();
        return result;
    }
};
void MediaPlayerPrivateScarlet::stopFramePump(bool preserveImage)
{
    if (!m_framePump) return;
    if (preserveImage) {
        // A paused/reparented video must retain its last compositor frame,
        // not the initial owner preview. This is a GPU-only immutable snapshot.
        auto state = m_framePump->snapshot();
        if (auto* context = PlatformDisplay::sharedDisplay().skiaGrContext()) {
            if (auto image = m_framePump->snapshotForOwner(context->threadSafeProxy())) {
                m_image = NativeImage::create(WTF::move(image), context);
                m_videoFrameSerial = std::max(m_videoFrameSerial, state.serial);
                m_snapshotVideoFrameSerial = m_videoFrameSerial;
                m_frameMediaTime = state.mediaTime;
            }
        }
    }
    auto pump = m_framePump.release();
    BinarySemaphore done;
    pump->runLoop->dispatch([pump, &done] {
        pump->timer.stop();
        delete pump;
        done.signal();
    });
    done.wait();
}
#endif
MediaPlayerPrivateScarlet::MediaPlayerPrivateScarlet(MediaPlayer& player)
    : m_player(player)
    , m_timer(RunLoop::currentSingleton(), "MediaPlayerPrivateScarlet::Poll"_s, this, &MediaPlayerPrivateScarlet::poll)
{
    m_status.paused = true;
}
MediaPlayerPrivateScarlet::~MediaPlayerPrivateScarlet()
{
    // MediaPlayer clears or replaces m_private before releasing this engine.
    // Cleanup must not notify a live player whose active engine is now null
    // or a different instance. Explicit cancelLoad still reports Empty.
    m_player = nullptr;
    cancelLoad();
}

void MediaPlayerPrivateScarlet::updateFrame()
{
    if (!m_session)
        return;
#if USE(COORDINATED_GRAPHICS)
    if (m_framePump) {
        auto state = m_framePump->snapshot();
        if (state.failed) { fail(MediaPlayer::NetworkState::DecodeError); return; }
        if (state.presentedCount > m_pumpPresentedCount && state.presentedSerial > m_paintedVideoFrameSerial) {
            m_presentedVideoFrames += state.presentedCount - m_pumpPresentedCount - 1;
            didPresentVideoFrame(state.presentedSerial, state.presentedMediaTime, state.presentedSize, state.presentedAt);
        }
        m_pumpPresentedCount = state.presentedCount;
        if (state.serial > m_videoFrameSerial) {
            m_videoFrameSerial = state.serial;
            m_frameMediaTime = state.mediaTime;
            if (m_size != state.size) {
                m_size = state.size;
                if (auto player = m_player.get()) player->sizeChanged();
            }
        }
        return;
    }
#endif
    auto* profile = mediaPollProfile();
    ScarletMediaFrame frame { };
    if (profileMediaPollPhase(profile, MediaPollPhase::Acquire, [&] { return scarlet_media_session_acquire_frame(m_session, &frame); }) <= 0)
        return;
    auto release = makeScopeExit([&] { profileMediaPollPhase(profile, MediaPollPhase::Release, [&] { scarlet_media_session_release_frame(m_session, frame.lease); }); });
    auto* context = PlatformDisplay::sharedDisplay().skiaGrContext();
    auto image = uploadVideoFrame(frame);
    if (!image) { fail(MediaPlayer::NetworkState::DecodeError); return; }
    profileMediaPollPhase(profile, MediaPollPhase::Replace, [&] { m_image = NativeImage::create(WTF::move(image), context); });
    ++m_videoFrameSerial;
    m_frameMediaTime = frame.presentation_us / 1000000.0;
    auto size = FloatSize { static_cast<float>(frame.width), static_cast<float>(frame.height) };
    if (auto player = m_player.get()) {
        if (m_size != size) {
            m_size = size;
            player->sizeChanged();
        }
        if (!m_notifiedFirstVideoFrame) {
            m_notifiedFirstVideoFrame = true;
            player->firstVideoFrameAvailable();
        }
        if (m_visible) {
#if USE(COORDINATED_GRAPHICS)
            if (!profileMediaPollPhase(profile, MediaPollPhase::Push, [&] { return pushFrameToCompositor(); }))
#endif
                player->repaint();
        }
    }
}

static unsigned boundedFrameCount(uint64_t count)
{
    return static_cast<unsigned>(std::min<uint64_t>(count, std::numeric_limits<unsigned>::max()));
}

ScarletMediaStatus MediaPlayerPrivateScarlet::statusSnapshot() const
{
    auto status = m_status;
    if (m_session)
        scarlet_media_session_status(m_session, &status);
    return status;
}
unsigned MediaPlayerPrivateScarlet::decodedFrameCount() const
{
    return boundedFrameCount(statusSnapshot().decoded_frames);
}
unsigned MediaPlayerPrivateScarlet::droppedFrameCount() const
{
    return boundedFrameCount(statusSnapshot().dropped_frames);
}
std::optional<VideoPlaybackQualityMetrics> MediaPlayerPrivateScarlet::videoPlaybackQualityMetrics()
{
    auto status = statusSnapshot();
    return VideoPlaybackQualityMetrics {
        boundedFrameCount(status.decoded_frames),
        boundedFrameCount(status.dropped_frames),
        0, 0,
        boundedFrameCount(m_presentedVideoFrames)
    };
}
std::optional<VideoFrameMetadata> MediaPlayerPrivateScarlet::videoFrameMetadata()
{
    return std::exchange(m_videoFrameMetadata, std::nullopt);
}
RefPtr<NativeImage> MediaPlayerPrivateScarlet::nativeImageForCurrentTime()
{
#if USE(COORDINATED_GRAPHICS)
    if (m_framePump && m_snapshotVideoFrameSerial != m_videoFrameSerial) {
        auto* context = PlatformDisplay::sharedDisplay().skiaGrContext();
        if (context) {
            if (auto image = m_framePump->snapshotForOwner(context->threadSafeProxy())) {
                m_image = NativeImage::create(WTF::move(image), context);
                m_snapshotVideoFrameSerial = m_videoFrameSerial;
            }
        }
    }
#endif
    return m_image;
}

void MediaPlayerPrivateScarlet::paint(GraphicsContext& context, const FloatRect& rectangle)
{
    auto image = nativeImageForCurrentTime();
    if (!image || context.paintingDisabled())
        return;
    context.drawNativeImage(*image, rectangle, FloatRect { FloatPoint { }, m_size });
    didPresentVideoFrame(m_videoFrameSerial, m_frameMediaTime, m_size, MonotonicTime::now());
}

void MediaPlayerPrivateScarlet::didPresentVideoFrame(uint64_t serial, double mediaTime, const FloatSize& size, MonotonicTime presentationTime)
{
    if (serial <= m_paintedVideoFrameSerial)
        return;
    m_paintedVideoFrameSerial = serial;
    ++m_presentedVideoFrames;
    if (!m_gatheringVideoFrameMetadata)
        return;
    auto now = presentationTime.secondsSinceEpoch().seconds();
    m_videoFrameMetadata = VideoFrameMetadata {
        .presentationTime = now,
        .expectedDisplayTime = now,
        .width = static_cast<unsigned>(size.width()),
        .height = static_cast<unsigned>(size.height()),
        .mediaTime = mediaTime,
        .presentedFrames = boundedFrameCount(m_presentedVideoFrames)
    };
    // Document keeps the VideoFrameCallbacks rendering step scheduled while
    // requests remain pending. This actual compositor submission is consumed
    // on the next step, without repainting the same image to make a callback.
}

void MediaPlayerPrivateScarlet::setPageIsVisible(bool visible)
{
    m_visible = visible;
#if USE(COORDINATED_GRAPHICS)
    if (!visible) stopFramePump();
    if (visible)
        pushFrameToCompositor();
#endif
}

#if USE(COORDINATED_GRAPHICS)
bool MediaPlayerPrivateScarlet::supportsAcceleratedRendering() const
{
    auto player = m_player.get();
    return (m_image || m_framePump) && player && player->acceleratedCompositingEnabled();
}

void MediaPlayerPrivateScarlet::acceleratedRenderingStateChanged()
{
    pushFrameToCompositor();
}

void MediaPlayerPrivateScarlet::setPlatformLayerBufferProxy(Ref<CoordinatedPlatformLayerBufferProxy>&& proxy)
{
    stopFramePump();
    m_contentsBufferProxy = WTF::move(proxy);
    ++m_compositorProxyGeneration;
    m_pushedVideoFrameSerial = 0;
    pushFrameToCompositor();
}

RefPtr<CoordinatedPlatformLayerBufferProxy> MediaPlayerPrivateScarlet::platformLayerBufferProxy() const
{
    return m_contentsBufferProxy;
}

void MediaPlayerPrivateScarlet::clearCompositorFrame()
{
    stopFramePump(false);
    ++m_videoPresentationGeneration;
    m_pushedVideoFrameSerial = 0;
    if (m_contentsBufferProxy && m_contentsBufferProxy->isValid())
        m_contentsBufferProxy->setDisplayBuffer(nullptr);
}

bool MediaPlayerPrivateScarlet::pushFrameToCompositor()
{
    auto player = m_player.get();
    if (!m_visible || !m_session || (!m_image && !m_framePump) || !m_contentsBufferProxy
        || !m_contentsBufferProxy->isValid() || !player || !player->acceleratedCompositingEnabled()) {
        stopFramePump();
        return false;
    }
    if (m_framePump) return true;
    auto loop = m_contentsBufferProxy->scarletCompositingRunLoop();
    if (!loop) return false;
    m_pumpPresentedCount = 0;
    m_framePump = std::make_unique<VideoFramePump>(*loop, m_session, *m_contentsBufferProxy, *this);
    // Publish the initial owner image as a promise for the compositor context.
    if (m_image) {
        auto image = m_image->platformImage();
        auto* context = PlatformDisplay::sharedDisplay().skiaGrContext();
        auto info = SkImageInfo::Make(image->width(), image->height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
        if (auto surface = SkSurfaces::RenderTarget(context, skgpu::Budgeted::kYes, info)) {
            surface->getCanvas()->drawImage(image, 0, 0);
            auto buffer = CoordinatedPlatformLayerBufferSkiaImage::create(surface->makeImageSnapshot(), m_contentsBufferProxy->threadSafeGrContext());
            m_contentsBufferProxy->setDisplayBuffer(WTF::move(buffer));
        }
    }
    auto* pump = m_framePump.get();
    loop->dispatch([pump] { pump->timer.startOneShot(0_s); });
    return true;
}
#endif

void MediaPlayerPrivateScarlet::play()
{
    if (m_session)
        scarlet_media_session_play(m_session);
#if USE(COORDINATED_GRAPHICS)
    if (auto* pump = m_framePump.get())
        pump->runLoop->dispatch([pump] { pump->timer.startOneShot(0_s); });
#endif
    wake();
}
void MediaPlayerPrivateScarlet::pause()
{
    if (m_session)
        scarlet_media_session_pause(m_session);
    wake();
}
void MediaPlayerPrivateScarlet::setRateDouble(double rate)
{
    m_rate = rate;
    if (m_session)
        scarlet_media_session_set_rate(m_session, rate);
    wake();
    if (auto player = m_player.get())
        player->rateChanged();
}
void MediaPlayerPrivateScarlet::setVolumeDouble(double volume)
{
    if (m_session)
        scarlet_media_session_set_volume(m_session, volume);
}
void MediaPlayerPrivateScarlet::setMuted(bool muted)
{
    if (m_session)
        scarlet_media_session_set_muted(m_session, muted);
}
MediaTime MediaPlayerPrivateScarlet::duration() const
{
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource)
        return m_mediaSource->duration();
#endif
    return MediaTime::createWithDouble(m_status.duration);
}
MediaTime MediaPlayerPrivateScarlet::currentTime() const
{
    if (m_seekTarget.isValid())
        return m_seekTarget;
    ScarletMediaStatus status { };
    if (m_session && scarlet_media_session_status(m_session, &status) >= 0)
        return MediaTime::createWithDouble(status.current_time);
    return MediaTime::zeroTime();
}
MediaPlayer::ReadyState MediaPlayerPrivateScarlet::readyState() const
{
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource)
        return m_mediaSource->mediaPlayerReadyState();
#endif
    return static_cast<MediaPlayer::ReadyState>(std::min<uint32_t>(m_status.ready_state, static_cast<uint32_t>(MediaPlayer::ReadyState::HaveEnoughData)));
}
const PlatformTimeRanges& MediaPlayerPrivateScarlet::buffered() const
{
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource)
        m_buffered = m_mediaSource->buffered();
#endif
    return m_buffered;
}
const PlatformTimeRanges& MediaPlayerPrivateScarlet::seekable() const
{
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource) {
        m_seekable = m_mediaSource->seekable();
        return m_seekable;
    }
#endif
    return buffered();
}

Ref<MediaTimePromise> MediaPlayerPrivateScarlet::seekToTarget(const SeekTarget& target)
{
    if (!m_session)
        return MediaTimePromise::createAndReject(PlatformMediaError::NotReady);
    bool isMediaSource = false;
#if ENABLE(MEDIA_SOURCE)
    isMediaSource = !!m_mediaSource;
#endif
    if (!isMediaSource && !m_buffered.contain(target.time) && (m_packets.isEmpty() || target.time != duration()))
        return MediaTimePromise::createAndReject(PlatformMediaError::NotReady);
    auto seekGeneration = ++m_seekGeneration;
    m_seekPromise.emplace(PlatformMediaError::Cancelled);
    auto seekPromise = m_seekPromise->promise();
    m_seekReadyForCompletion = false;
    m_seekTarget = target.time;
    scarlet_media_session_seek(m_session, target.time.toDouble());
#if USE(COORDINATED_GRAPHICS)
    clearCompositorFrame();
#endif
    m_image = nullptr;
    m_videoFrameMetadata.reset();
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource) {
        Ref protectedThis { *this };
        Ref source { *m_mediaSource };
        source->waitForTarget(target)->whenSettled(RunLoop::currentSingleton(), [protectedThis, source, seekGeneration](auto&& result) {
            if (protectedThis->m_seekGeneration != seekGeneration || protectedThis->m_mediaSource.get() != source.ptr())
                return;
            if (!result) {
                protectedThis->m_seekTarget = MediaTime::invalidTime();
                if (auto promise = std::exchange(protectedThis->m_seekPromise, std::nullopt))
                    promise->reject(PlatformMediaError::Cancelled);
                return;
            }
            source->reenqueueMediaForTime(*result)->whenSettled(RunLoop::currentSingleton(), [protectedThis, source, seekGeneration](auto&& result) {
                if (protectedThis->m_seekGeneration != seekGeneration || protectedThis->m_mediaSource.get() != source.ptr())
                    return;
                if (!result) {
                    protectedThis->m_seekTarget = MediaTime::invalidTime();
                    if (auto promise = std::exchange(protectedThis->m_seekPromise, std::nullopt))
                        promise->reject(PlatformMediaError::Cancelled);
                }
                protectedThis->m_seekReadyForCompletion = !!result;
                protectedThis->wake();
            });
        });
        wake();
        return seekPromise;
    }
#endif
    MediaTime start = MediaTime::zeroTime();
    for (auto& sample : m_packets) {
        auto track = m_tracks.find(sample->trackID());
        if (track != m_tracks.end() && track->value.kind == SCARLET_MEDIA_VIDEO && sample->isSync() && sample->presentationTime() <= target.time)
            start = sample->decodeTime();
    }
    m_replay.clear();
    m_replayPending = true;
    for (auto& sample : m_packets) {
        if (sample->decodeTime() >= start)
            m_replay.append(sample.copyRef());
    }
    m_seekReadyForCompletion = true;
    wake();
    return seekPromise;
}
#if ENABLE(MEDIA_SOURCE)
void MediaPlayerPrivateScarlet::load(const URL&, const LoadOptions&, MediaSourcePrivateClient& client)
{
    cancelLoad();
    m_session = scarlet_media_session_create();
    if (!m_session) {
        fail(MediaPlayer::NetworkState::DecodeError);
        return;
    }
    if (auto player = m_player.get()) {
        m_rate = player->requestedRate();
        scarlet_media_session_set_rate(m_session, m_rate);
        scarlet_media_session_set_volume(m_session, player->volume());
        scarlet_media_session_set_muted(m_session, player->muted());
    }
    m_mediaSource = MediaSourcePrivateScarlet::open(client, *this);
    // SourceBuffer accepts JavaScript-visible bytes, as for the AVFoundation
    // MSE engine. Network-origin checks happened before JS could read them.
    m_passedCors = true;
    setNetworkState(MediaPlayer::NetworkState::Loading);
    wake();
}
void MediaPlayerPrivateScarlet::readyStateFromMediaSourceChanged()
{
    if (auto player = m_player.get())
        player->readyStateChanged();
    wake();
}
void MediaPlayerPrivateScarlet::characteristicsFromMediaSourceChanged()
{
    if (auto player = m_player.get())
        player->characteristicChanged();
}
void MediaPlayerPrivateScarlet::mediaSourceDurationChanged()
{
    if (auto player = m_player.get())
        player->durationChanged();
}
void MediaPlayerPrivateScarlet::mediaSourceEndOfStream(bool networkError, bool decodeError)
{
    // Detail: 0=normal EOS, 1=network error, 2=decode error.
    auto detail = networkError ? 1 : decodeError ? 2 : 0;
    profileMediaLifecycle("mse-eos-before", m_session, m_networkState, m_inputBytes,
        true, m_inputFinished, m_endSubmitted, m_demuxEnded, detail);
    if (networkError)
        fail(MediaPlayer::NetworkState::NetworkError);
    else if (decodeError)
        fail(MediaPlayer::NetworkState::DecodeError);
    else
        setNetworkState(MediaPlayer::NetworkState::Loaded);
    profileMediaLifecycle("mse-eos-after", m_session, m_networkState, m_inputBytes,
        true, m_inputFinished, m_endSubmitted, m_demuxEnded, detail);
    wake();
}
void MediaPlayerPrivateScarlet::seekableRangesFromMediaSourceChanged()
{
    if (auto player = m_player.get())
        player->timeChanged();
}
#endif
}
#endif
