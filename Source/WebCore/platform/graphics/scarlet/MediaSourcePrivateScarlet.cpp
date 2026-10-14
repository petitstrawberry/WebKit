/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "config.h"
#include "MediaSourcePrivateScarlet.h"
#if USE(SCARLET_MEDIA) && ENABLE(MEDIA_SOURCE)
#include "ContentType.h"
#include "MediaPlayerPrivateScarlet.h"
#include "MediaSourcePrivateClient.h"
#include "SourceBufferPrivateScarlet.h"
namespace WebCore {
Ref<MediaSourcePrivateScarlet> MediaSourcePrivateScarlet::open(MediaSourcePrivateClient& client, MediaPlayerPrivateScarlet& player)
{
    auto source = adoptRef(*new MediaSourcePrivateScarlet(client, player));
    client.setPrivateAndOpen(source.copyRef());
    return source;
}
MediaSourcePrivateScarlet::MediaSourcePrivateScarlet(MediaSourcePrivateClient& client, MediaPlayerPrivateScarlet& player)
    : MediaSourcePrivate(client), m_player(player)
{
}
RefPtr<MediaPlayerPrivateScarlet> MediaSourcePrivateScarlet::scarletPlayer() const { return m_player.get(); }
RefPtr<MediaPlayerPrivateInterface> MediaSourcePrivateScarlet::player() const { return m_player.get(); }
void MediaSourcePrivateScarlet::setPlayer(MediaPlayerPrivateInterface* player)
{
    m_player = player ? static_cast<MediaPlayerPrivateScarlet*>(player) : nullptr;
}
MediaSourcePrivate::AddStatus MediaSourcePrivateScarlet::addSourceBuffer(const ContentType& type, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>& output)
{
    auto mime = type.containerType().utf8();
    auto codecs = type.parameter(ContentType::codecsParameter()).utf8();
    if (scarlet_media_supports_type(reinterpret_cast<const char*>(mime.data()), mime.length(), reinterpret_cast<const char*>(codecs.data()), codecs.length(), true) <= 0)
        return AddStatus::NotSupported;
    if (!m_player.get() || sourceBuffers().size() >= 8)
        return AddStatus::ReachedIdLimit;
    auto buffer = SourceBufferPrivateScarlet::create(*this, type);
    if (!buffer)
        return AddStatus::NotSupported;
    {
        Locker locker { m_lock };
        m_sourceBuffers.append(buffer);
    }
    output = WTF::move(buffer);
    return AddStatus::Ok;
}
void MediaSourcePrivateScarlet::notifyActiveSourceBuffersChanged()
{
    if (auto player = m_player.get())
        player->characteristicsFromMediaSourceChanged();
}
void MediaSourcePrivateScarlet::durationChanged(const MediaTime& time)
{
    MediaSourcePrivate::durationChanged(time);
    if (auto player = m_player.get())
        player->mediaSourceDurationChanged();
}
void MediaSourcePrivateScarlet::markEndOfStream(EndOfStreamStatus status)
{
    MediaSourcePrivate::markEndOfStream(status);
    for (auto& source : sourceBuffers())
        static_cast<SourceBufferPrivateScarlet&>(source.get()).endOfStream();
    if (auto player = m_player.get())
        player->mediaSourceEndOfStream(status == EndOfStreamStatus::NetworkError, status == EndOfStreamStatus::DecodeError);
}
void MediaSourcePrivateScarlet::notifyDecodersReady()
{
    for (auto& source : sourceBuffers())
        static_cast<SourceBufferPrivateScarlet&>(source.get()).notifyDecodersReady();
}
}
#endif
