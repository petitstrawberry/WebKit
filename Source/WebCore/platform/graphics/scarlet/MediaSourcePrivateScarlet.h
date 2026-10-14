/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#if USE(SCARLET_MEDIA) && ENABLE(MEDIA_SOURCE)
#include "MediaSourcePrivate.h"
#include <wtf/ThreadSafeWeakPtr.h>
namespace WebCore {
class MediaPlayerPrivateScarlet;
class MediaSourcePrivateScarlet final : public MediaSourcePrivate {
public:
    static Ref<MediaSourcePrivateScarlet> open(MediaSourcePrivateClient&, MediaPlayerPrivateScarlet&);
    RefPtr<MediaPlayerPrivateInterface> player() const final;
    RefPtr<MediaPlayerPrivateScarlet> scarletPlayer() const;
    void setPlayer(MediaPlayerPrivateInterface*) final;
    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::Scarlet; }
    AddStatus addSourceBuffer(const ContentType&, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>&) final;
    void notifyActiveSourceBuffersChanged() final;
    void durationChanged(const MediaTime&) final;
    void markEndOfStream(EndOfStreamStatus) final;
    TrackID allocateTrackIdentifier() { return m_nextTrackIdentifier++; }
    void notifyDecodersReady();
private:
    MediaSourcePrivateScarlet(MediaSourcePrivateClient&, MediaPlayerPrivateScarlet&);
    ThreadSafeWeakPtr<MediaPlayerPrivateScarlet> m_player;
    TrackID m_nextTrackIdentifier { 1 };
};
}
#endif
