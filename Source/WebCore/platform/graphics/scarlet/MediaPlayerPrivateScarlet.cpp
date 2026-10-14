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

namespace WebCore {
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
    void responseReceived(PlatformMediaResource&, const ResourceResponse& response, CompletionHandler<void(ShouldContinuePolicyCheck)>&& completion) final
    {
        if (auto player = m_player.get()) {
            player->receiveResponse(response);
            completion(player->networkState() == MediaPlayer::NetworkState::Loading ? ShouldContinuePolicyCheck::Yes : ShouldContinuePolicyCheck::No);
        } else
            completion(ShouldContinuePolicyCheck::No);
    }
    void dataReceived(PlatformMediaResource&, const SharedBuffer& data) final
    {
        if (auto player = m_player.get())
            player->receiveData(data);
    }
    void loadFinished(PlatformMediaResource&, const NetworkLoadMetrics&) final
    {
        if (auto player = m_player.get())
            player->finishLoading();
    }
    void loadFailed(PlatformMediaResource&, const ResourceError&) final
    {
        if (auto player = m_player.get())
            player->fail(MediaPlayer::NetworkState::NetworkError);
    }
    void accessControlCheckFailed(PlatformMediaResource&, const ResourceError&) final
    {
        if (auto player = m_player.get())
            player->fail(MediaPlayer::NetworkState::NetworkError);
    }
private:
    ThreadSafeWeakPtr<MediaPlayerPrivateScarlet> m_player;
};

MediaPlayerPrivateScarlet::MediaPlayerPrivateScarlet(MediaPlayer& player)
    : m_player(player)
    , m_timer(RunLoop::currentSingleton(), "MediaPlayerPrivateScarlet::Poll"_s, this, &MediaPlayerPrivateScarlet::poll)
{
    m_status.paused = true;
}
MediaPlayerPrivateScarlet::~MediaPlayerPrivateScarlet()
{
    cancelLoad();
}

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
    m_image = nullptr;
    m_size = { };
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
        m_image = nullptr;
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
            fail(MediaPlayer::NetworkState::DecodeError);
            return;
        } else if (event.kind == SCARLET_MEDIA_APPEND_DONE && m_endSubmitted && event.append_identifier == m_appendIdentifier) {
            m_demuxEnded = true;
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
    pumpInput();
    pumpParser();
    updateStatus();
    updateFrame();
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource)
        m_mediaSource->notifyDecodersReady();
#endif
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
    if (m_seekPromise && m_seekReadyForCompletion && status.ready_state >= static_cast<unsigned>(MediaPlayer::ReadyState::HaveCurrentData)) {
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
void MediaPlayerPrivateScarlet::updateFrame()
{
    if (!m_session)
        return;
    ScarletMediaFrame frame { };
    if (scarlet_media_session_acquire_frame(m_session, &frame) <= 0)
        return;
    auto release = makeScopeExit([&] { scarlet_media_session_release_frame(m_session, frame.lease); });
    if (!frame.width || !frame.height || frame.width > 16384 || frame.height > 16384
        || (frame.format != SCARLET_MEDIA_I420 && frame.format != SCARLET_MEDIA_NV12) || !prepareSkiaContext()) {
        fail(MediaPlayer::NetworkState::DecodeError);
        return;
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
            fail(MediaPlayer::NetworkState::DecodeError);
            return;
        }
        auto imageInfo = SkImageInfo::Make(width, height, channels == 2 ? kR8G8_unorm_SkColorType : kGray_8_SkColorType, kOpaque_SkAlphaType);
        planes[i].reset(imageInfo, frame.planes[i], frame.strides[i]);
    }
    auto pixmaps = SkYUVAPixmaps::FromExternalPixmaps(info, planes);
    auto image = SkImages::TextureFromYUVAPixmaps(context, pixmaps, skgpu::Mipmapped::kNo, false, SkColorSpace::MakeSRGB());
    if (!image) {
        fail(MediaPlayer::NetworkState::DecodeError);
        return;
    }
    m_image = NativeImage::create(WTF::move(image), context);
    auto size = FloatSize { static_cast<float>(frame.width), static_cast<float>(frame.height) };
    if (auto player = m_player.get()) {
        if (m_size != size) {
            m_size = size;
            player->sizeChanged();
        }
        if (m_visible)
            player->repaint();
    }
}

void MediaPlayerPrivateScarlet::paint(GraphicsContext& context, const FloatRect& rectangle)
{
    if (m_image)
        context.drawNativeImage(*m_image, rectangle, FloatRect { FloatPoint { }, m_size });
}
void MediaPlayerPrivateScarlet::play()
{
    if (m_session)
        scarlet_media_session_play(m_session);
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
    auto seekGeneration = ++m_seekGeneration;
    m_seekPromise.emplace(PlatformMediaError::Cancelled);
    auto seekPromise = m_seekPromise->promise();
    m_seekReadyForCompletion = false;
    m_seekTarget = target.time;
    scarlet_media_session_seek(m_session, target.time.toDouble());
    m_image = nullptr;
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
    if (!m_buffered.contain(target.time) && (m_packets.isEmpty() || target.time != duration())) {
        m_seekTarget = MediaTime::invalidTime();
        m_seekPromise.reset();
        return MediaTimePromise::createAndReject(PlatformMediaError::NotReady);
    }
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
    if (networkError)
        fail(MediaPlayer::NetworkState::NetworkError);
    else if (decodeError)
        fail(MediaPlayer::NetworkState::DecodeError);
    else
        setNetworkState(MediaPlayer::NetworkState::Loaded);
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
