/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScarletMediaParser ScarletMediaParser;
typedef struct ScarletMediaSession ScarletMediaSession;

/* All input bytes are copied before a successful call returns. No WebCore
 * object or callback crosses the decoder thread. Zero is success, one is
 * bounded-queue backpressure, negative values are permanent failures. */
enum { SCARLET_MEDIA_MAX_APPEND_BYTES = 512 * 1024 };
enum ScarletMediaResult { SCARLET_MEDIA_OK = 0, SCARLET_MEDIA_AGAIN = 1, SCARLET_MEDIA_ERROR = -1 };
enum ScarletMediaCodec { SCARLET_MEDIA_AVC = 1, SCARLET_MEDIA_AAC = 2 };
enum ScarletMediaTrackKind { SCARLET_MEDIA_VIDEO = 1, SCARLET_MEDIA_AUDIO = 2 };
enum ScarletMediaParserEventKind {
    SCARLET_MEDIA_TRACK = 1, SCARLET_MEDIA_SAMPLE = 2,
    SCARLET_MEDIA_APPEND_DONE = 3, SCARLET_MEDIA_PARSE_ERROR = 4
};
enum ScarletMediaSampleFlags { SCARLET_MEDIA_SYNC = 1, SCARLET_MEDIA_NON_DISPLAYING = 2 };
enum ScarletMediaPixelFormat { SCARLET_MEDIA_I420 = 1, SCARLET_MEDIA_NV12 = 2 };
enum ScarletMediaMatrix { SCARLET_MEDIA_BT601 = 1, SCARLET_MEDIA_BT709 = 2, SCARLET_MEDIA_BT2020 = 3 };

typedef struct {
    uint64_t identifier;
    uint32_t kind;
    uint32_t codec;
    uint32_t width;
    uint32_t height;
    uint32_t sample_rate;
    uint32_t channels;
    double duration;
    const uint8_t* configuration;
    size_t configuration_size;
} ScarletMediaTrack;

typedef struct {
    uint64_t track;
    int64_t presentation_us;
    int64_t decode_us;
    int64_t duration_us;
    uint32_t flags;
    uint32_t reserved;
    const uint8_t* data;
    size_t size;
} ScarletMediaSample;

typedef struct {
    uint64_t lease;
    uint64_t append_identifier;
    uint32_t kind;
    int32_t error;
    ScarletMediaTrack track;
    ScarletMediaSample sample;
} ScarletMediaParserEvent;

typedef struct {
    uint64_t lease;
    int64_t presentation_us;
    int64_t duration_us;
    uint32_t format;
    uint32_t matrix;
    uint32_t full_range;
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
    const uint8_t* planes[3];
    size_t strides[3];
    size_t plane_sizes[3];
} ScarletMediaFrame;

typedef struct {
    double current_time;
    double duration;
    uint64_t decoded_frames;
    uint64_t dropped_frames;
    uint32_t has_audio;
    uint32_t has_video;
    uint32_t paused;
    uint32_t ended;
    uint32_t ready_state; /* HTMLMediaElement HAVE_NOTHING .. HAVE_ENOUGH_DATA */
    int32_t error;
} ScarletMediaStatus;

/* Capability must describe the actual installed demux/decoder combination.
 * is_media_source requests incremental initialization/media segment support. */
int32_t scarlet_media_supports_type(const char* mime, size_t mime_size,
    const char* codecs, size_t codecs_size, uint32_t is_media_source);
ScarletMediaParser* scarlet_media_parser_create(const char* mime, size_t mime_size, uint32_t is_media_source);
void scarlet_media_parser_destroy(ScarletMediaParser*);
int32_t scarlet_media_parser_append(ScarletMediaParser*, const uint8_t*, size_t, uint64_t append_identifier);
int32_t scarlet_media_parser_end(ScarletMediaParser*, uint64_t append_identifier);
void scarlet_media_parser_reset(ScarletMediaParser*);
/* 0: no event, 1: event acquired, negative: failure. Event pointers remain
 * valid until release; every acquired lease must be released once. */
int32_t scarlet_media_parser_poll(ScarletMediaParser*, ScarletMediaParserEvent*);
void scarlet_media_parser_release(ScarletMediaParser*, uint64_t lease);

ScarletMediaSession* scarlet_media_session_create(void);
void scarlet_media_session_destroy(ScarletMediaSession*);
int32_t scarlet_media_session_configure(ScarletMediaSession*, const ScarletMediaTrack*);
int32_t scarlet_media_session_enqueue(ScarletMediaSession*, const ScarletMediaSample*);
uint32_t scarlet_media_session_has_capacity(ScarletMediaSession*, uint64_t track);
void scarlet_media_session_flush(ScarletMediaSession*, uint64_t track);
/* Removal destroys track configuration and invalidates its decoder generation;
 * flushing preserves configuration for seek and coded-frame overlap. */
void scarlet_media_session_remove_track(ScarletMediaSession*, uint64_t track);
void scarlet_media_session_end(ScarletMediaSession*, uint64_t track);
void scarlet_media_session_play(ScarletMediaSession*);
void scarlet_media_session_pause(ScarletMediaSession*);
void scarlet_media_session_set_rate(ScarletMediaSession*, double);
void scarlet_media_session_set_volume(ScarletMediaSession*, double);
void scarlet_media_session_set_muted(ScarletMediaSession*, uint32_t);
/* Flush the generation and reset audio/video clocks to the requested time.
 * Encoded samples must then be reenqueued from the preceding random access
 * point; the bridge must not synthesize decoded frames or seek completion. */
void scarlet_media_session_seek(ScarletMediaSession*, double);
int32_t scarlet_media_session_status(ScarletMediaSession*, ScarletMediaStatus*);
/* Returns only frames due at the actual playback clock. */
int32_t scarlet_media_session_acquire_frame(ScarletMediaSession*, ScarletMediaFrame*);
void scarlet_media_session_release_frame(ScarletMediaSession*, uint64_t lease);

#ifdef __cplusplus
}
#endif
