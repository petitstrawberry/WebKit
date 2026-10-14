/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#if USE(SCARLET_MEDIA)
#include "MediaSample.h"
#include "ScarletMedia.h"
#include <wtf/Vector.h>
namespace WebCore {
class MediaSampleScarlet final : public MediaSample {
public:
    static Ref<MediaSampleScarlet> create(const ScarletMediaSample& sample, const FloatSize& size = { })
    {
        return adoptRef(*new MediaSampleScarlet(sample, size));
    }
    MediaTime presentationTime() const final { return { m_sample.presentation_us, 1000000 }; }
    MediaTime decodeTime() const final { return { m_sample.decode_us, 1000000 }; }
    MediaTime duration() const final { return { m_sample.duration_us, 1000000 }; }
    TrackID trackID() const final { return m_sample.track; }
    size_t sizeInBytes() const final { return m_data.size(); }
    FloatSize presentationSize() const final { return m_size; }
    void offsetTimestampsBy(const MediaTime& offset) final
    {
        auto us = offset.toTimeScale(1000000).timeValue();
        m_sample.presentation_us += us;
        m_sample.decode_us += us;
    }
    void setTimestamps(const MediaTime& presentation, const MediaTime& decode) final
    {
        m_sample.presentation_us = presentation.toTimeScale(1000000).timeValue();
        m_sample.decode_us = decode.toTimeScale(1000000).timeValue();
    }
    Ref<MediaSample> createNonDisplayingCopy() const final
    {
        auto sample = bridgeSample();
        sample.flags |= SCARLET_MEDIA_NON_DISPLAYING;
        return create(sample, m_size);
    }
    Ref<MediaSample> createCopyWithAdjustedStartTime(const MediaTime& offset) const final
    {
        auto sample = bridgeSample();
        auto us = std::clamp<int64_t>(offset.toTimeScale(1000000).timeValue(), 0, sample.duration_us);
        sample.presentation_us += us;
        sample.decode_us += us;
        sample.duration_us -= us;
        return create(sample, m_size);
    }
    SampleFlags flags() const final
    {
        unsigned flags = HasSyncInfo;
        if (m_sample.flags & SCARLET_MEDIA_SYNC)
            flags |= IsSync;
        if (m_sample.flags & SCARLET_MEDIA_NON_DISPLAYING)
            flags |= IsNonDisplaying;
        return static_cast<SampleFlags>(flags);
    }
    PlatformSample platformSample() const final { return PlatformSample { this }; }
    Type type() const final { return Type::Scarlet; }
    ScarletMediaSample bridgeSample() const
    {
        auto sample = m_sample;
        sample.data = m_data.span().data();
        sample.size = m_data.size();
        return sample;
    }
private:
    MediaSampleScarlet(const ScarletMediaSample& sample, const FloatSize& size)
        : m_sample(sample), m_size(size)
    {
        m_data.append(std::span { sample.data, sample.size });
        m_sample.data = nullptr;
    }
    ScarletMediaSample m_sample;
    FloatSize m_size;
    Vector<uint8_t> m_data;
};
}
SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::MediaSampleScarlet)
static bool isType(const WebCore::MediaSample& sample) { return sample.type() == WebCore::MediaSample::Type::Scarlet; }
SPECIALIZE_TYPE_TRAITS_END()
#endif
