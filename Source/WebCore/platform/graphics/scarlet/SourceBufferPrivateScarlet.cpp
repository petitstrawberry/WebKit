/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "config.h"
#include "SourceBufferPrivateScarlet.h"
#if USE(SCARLET_MEDIA) && ENABLE(MEDIA_SOURCE)
#include "ContentType.h"
#include "Logging.h"
#include "MediaPlayerPrivateScarlet.h"
#include "MediaSourcePrivateScarlet.h"
#include "MediaSampleScarlet.h"
#include "SharedBuffer.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/Scope.h>
namespace WebCore {
class MediaDescriptionScarlet final : public MediaDescription {
public:
    static Ref<MediaDescriptionScarlet> create(const ScarletMediaTrack& track) { return adoptRef(*new MediaDescriptionScarlet(track)); }
    bool isVideo() const final { return m_kind == SCARLET_MEDIA_VIDEO; }
    bool isAudio() const final { return m_kind == SCARLET_MEDIA_AUDIO; }
    bool isText() const final { return false; }
private:
    MediaDescriptionScarlet(const ScarletMediaTrack& track)
        : MediaDescription(track.codec == SCARLET_MEDIA_AVC ? "avc1"_s : "mp4a.40.2"_s), m_kind(track.kind) { }
    uint32_t m_kind;
};
class VideoTrackPrivateScarlet final : public VideoTrackPrivate {
public:
    static Ref<VideoTrackPrivateScarlet> create(TrackID identifier) { return adoptRef(*new VideoTrackPrivateScarlet(identifier)); }
    TrackID id() const final { return m_identifier; }
    Kind kind() const final { return Kind::Main; }
private:
    explicit VideoTrackPrivateScarlet(TrackID identifier) : m_identifier(identifier) { }
    TrackID m_identifier;
};
class AudioTrackPrivateScarlet final : public AudioTrackPrivate {
public:
    static Ref<AudioTrackPrivateScarlet> create(TrackID identifier) { return adoptRef(*new AudioTrackPrivateScarlet(identifier)); }
    TrackID id() const final { return m_identifier; }
    Kind kind() const final { return Kind::Main; }
private:
    explicit AudioTrackPrivateScarlet(TrackID identifier) : m_identifier(identifier) { }
    TrackID m_identifier;
};
RefPtr<SourceBufferPrivateScarlet> SourceBufferPrivateScarlet::create(MediaSourcePrivateScarlet& source, const ContentType& type)
{
    auto mime = type.containerType().utf8();
    auto* parser = scarlet_media_parser_create(reinterpret_cast<const char*>(mime.data()), mime.length(), true);
    if (!parser)
        return nullptr;
    return adoptRef(*new SourceBufferPrivateScarlet(source, parser));
}
SourceBufferPrivateScarlet::SourceBufferPrivateScarlet(MediaSourcePrivateScarlet& source, ScarletMediaParser* parser)
    : SourceBufferPrivate(source), m_parent(source), m_parser(parser)
    , m_timer(RunLoop::currentSingleton(), "SourceBufferPrivateScarlet::Parser"_s, this, &SourceBufferPrivateScarlet::poll)
{
}
SourceBufferPrivateScarlet::~SourceBufferPrivateScarlet()
{
    m_timer.stop();
    m_appendPromise.reset();
    scarlet_media_parser_destroy(m_parser);
}
RefPtr<MediaPlayerPrivateScarlet> SourceBufferPrivateScarlet::player() const
{
    auto parent = m_parent.get();
    return parent ? parent->scarletPlayer() : nullptr;
}
Ref<MediaPromise> SourceBufferPrivateScarlet::appendInternal(Ref<SharedBuffer>&& bytes)
{
    if (m_appendPromise)
        return MediaPromise::createAndReject(PlatformMediaError::InvalidState);
    m_appendPromise.emplace(PlatformMediaError::BufferRemoved);
    m_appendData = WTF::move(bytes);
    m_appendOffset = 0;
    m_outstandingAppends = 0;
    m_initialization.reset();
    auto promise = m_appendPromise->promise();
    submitInput();
    if (!m_timer.isActive())
        m_timer.startOneShot(0_s);
    return promise;
}
void SourceBufferPrivateScarlet::submitInput()
{
    if (!m_appendData)
        return;
    auto bytes = m_appendData->span();
    // Each accepted parser append has its own completion identifier. Large JS
    // appends are streamed to the bounded bridge without holding extra copies.
    do {
        auto size = std::min<size_t>(bytes.size() - m_appendOffset, SCARLET_MEDIA_MAX_APPEND_BYTES);
        auto result = scarlet_media_parser_append(m_parser, bytes.data() + m_appendOffset, size, m_appendIdentifier + 1);
        if (result == SCARLET_MEDIA_AGAIN)
            return;
        if (result != SCARLET_MEDIA_OK) {
            finishAppend(false);
            return;
        }
        ++m_appendIdentifier;
        ++m_outstandingAppends;
        m_appendOffset += size;
    } while (m_appendOffset < bytes.size());
    m_appendData = nullptr;
}
void SourceBufferPrivateScarlet::receiveTrack(const ScarletMediaTrack& parsedTrack)
{
    auto parent = m_parent.get();
    auto nativePlayer = player();
    if (!parent || !nativePlayer) {
        finishAppend(false);
        return;
    }
    auto track = parsedTrack;
    track.identifier = m_trackIdentifiers.ensure(parsedTrack.identifier, [&] { return parent->allocateTrackIdentifier(); }).iterator->value;
    if (!nativePlayer->configureTrack(track)) {
        finishAppend(false);
        return;
    }
    if (!m_initialization) {
        m_initialization = InitializationSegment { };
        m_initialization->duration = MediaTime::positiveInfiniteTime();
    }
    if (track.duration > 0)
        m_initialization->duration = MediaTime::createWithDouble(track.duration);
    else if (!m_initialization->duration.isValid())
        m_initialization->duration = MediaTime::positiveInfiniteTime();
    auto description = MediaDescriptionScarlet::create(track);
    if (track.kind == SCARLET_MEDIA_VIDEO)
        m_initialization->videoTracks.append({ WTF::move(description), VideoTrackPrivateScarlet::create(track.identifier) });
    else if (track.kind == SCARLET_MEDIA_AUDIO)
        m_initialization->audioTracks.append({ WTF::move(description), AudioTrackPrivateScarlet::create(track.identifier) });
}
void SourceBufferPrivateScarlet::deliverInitializationSegment()
{
    if (auto initialization = std::exchange(m_initialization, std::nullopt))
        didReceiveInitializationSegment(WTF::move(*initialization));
}
void SourceBufferPrivateScarlet::poll()
{
    Ref protectedThis { *this };
    submitInput();
    if (!m_appendPromise)
        return;
    for (unsigned count = 0; count < 64; ++count) {
        ScarletMediaParserEvent event { };
        auto result = scarlet_media_parser_poll(m_parser, &event);
        if (!result)
            break;
        if (result < 0) {
            finishAppend(false);
            return;
        }
        auto release = makeScopeExit([&] { scarlet_media_parser_release(m_parser, event.lease); });
        if (event.kind == SCARLET_MEDIA_TRACK)
            receiveTrack(event.track);
        else if (event.kind == SCARLET_MEDIA_SAMPLE) {
            deliverInitializationSegment();
            auto identifier = m_trackIdentifiers.find(event.sample.track);
            if (identifier == m_trackIdentifiers.end()) {
                finishAppend(false);
                return;
            }
            event.sample.track = identifier->value;
            didReceiveSample(MediaSampleScarlet::create(event.sample));
        } else if (event.kind == SCARLET_MEDIA_APPEND_DONE) {
            if (!m_outstandingAppends) {
                finishAppend(false);
                return;
            }
            --m_outstandingAppends;
        } else {
            finishAppend(false);
            return;
        }
        if (!m_appendPromise)
            return;
    }
    submitInput();
    if (m_appendPromise && !m_appendData && !m_outstandingAppends) {
        deliverInitializationSegment();
        finishAppend(true);
    } else if (m_appendPromise)
        m_timer.startOneShot(Seconds { 1.0 / 60 });
}
void SourceBufferPrivateScarlet::finishAppend(bool success)
{
    m_timer.stop();
    m_appendData = nullptr;
    if (auto promise = std::exchange(m_appendPromise, std::nullopt)) {
        if (success)
            promise->resolve();
        else
            promise->reject(PlatformMediaError::ParsingError);
    }
}
void SourceBufferPrivateScarlet::resetParserStateInternal()
{
    finishAppend(false);
    m_initialization.reset();
    m_outstandingAppends = 0;
    scarlet_media_parser_reset(m_parser);
}
void SourceBufferPrivateScarlet::removedFromMediaSource()
{
    resetParserStateInternal();
    m_deferredSample = nullptr;
    if (auto nativePlayer = player()) {
        for (auto& [local, identifier] : m_trackIdentifiers)
            nativePlayer->flushTrack(identifier);
    }
    m_parent = nullptr;
    SourceBufferPrivate::removedFromMediaSource();
}
void SourceBufferPrivateScarlet::flush(TrackID identifier)
{
    if (m_deferredSample && m_deferredSample->trackID() == identifier)
        m_deferredSample = nullptr;
    if (auto nativePlayer = player())
        nativePlayer->flushTrack(identifier);
}
void SourceBufferPrivateScarlet::enqueueSample(Ref<MediaSample>&& sample, TrackID)
{
    auto nativePlayer = player();
    if (!nativePlayer || !is<MediaSampleScarlet>(sample))
        return;
    Ref packet = downcast<MediaSampleScarlet>(WTF::move(sample));
    if (!nativePlayer->enqueueSample(packet))
        m_deferredSample = WTF::move(packet);
}
bool SourceBufferPrivateScarlet::isReadyForMoreSamples(TrackID identifier)
{
    auto nativePlayer = player();
    return nativePlayer && !m_deferredSample && nativePlayer->hasCapacity(identifier);
}
void SourceBufferPrivateScarlet::notifyClientWhenReadyForMoreSamples(TrackID identifier)
{
    m_waitingTracks.add(identifier);
    if (auto nativePlayer = player())
        nativePlayer->wake();
}
void SourceBufferPrivateScarlet::notifyDecodersReady()
{
    auto nativePlayer = player();
    if (!nativePlayer)
        return;
    if (m_deferredSample && nativePlayer->enqueueSample(*m_deferredSample))
        m_deferredSample = nullptr;
    if (m_deferredSample)
        return;
    Vector<TrackID> ready;
    for (auto identifier : m_waitingTracks) {
        if (nativePlayer->hasCapacity(identifier))
            ready.append(identifier);
    }
    for (auto identifier : ready) {
        m_waitingTracks.remove(identifier);
        provideMediaData(identifier);
    }
}
void SourceBufferPrivateScarlet::allSamplesInTrackEnqueued(TrackID)
{
    // Exhausting one append is not end-of-stream: more MSE segments may arrive.
    if (auto nativePlayer = player())
        nativePlayer->wake();
}
void SourceBufferPrivateScarlet::endOfStream()
{
    if (auto nativePlayer = player()) {
        for (auto& [local, identifier] : m_trackIdentifiers)
            nativePlayer->endTrack(identifier);
    }
}
#if !RELEASE_LOG_DISABLED
const Logger& SourceBufferPrivateScarlet::sourceBufferLogger() const
{
    static NeverDestroyed<Ref<Logger>> logger { Logger::create(this) };
    return logger.get().get();
}
#endif
}
#endif
