/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* C ABI implemented by the Scarlet-native bridge in
 * crates/myrica-webkit-sys/src/cdm_bridge.rs. The Widevine CDM itself runs in
 * a Linux ABI view; WebCore never loads it in this process. Buffers returned
 * through ScarletWidevineBuffer are owned by Rust and must be released with
 * cdm_host_buffer_free. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScarletWidevineHandle ScarletWidevineHandle;

typedef struct {
    const uint8_t* data;
    size_t length;
} ScarletWidevineBytes;

typedef struct {
    uint8_t* data;
    size_t length;
} ScarletWidevineBuffer;

enum {
    SCARLET_WIDEVINE_OK = 0,
    SCARLET_WIDEVINE_ERROR = -1,
    SCARLET_WIDEVINE_NO_KEY = -2,
    SCARLET_WIDEVINE_PROTOCOL = -3,
    SCARLET_WIDEVINE_SPAWN = -4,
};

/* Event kinds queued by the host. */
enum {
    SCARLET_WIDEVINE_EVENT_MESSAGE = 103,
    SCARLET_WIDEVINE_EVENT_KEYS = 104,
    SCARLET_WIDEVINE_EVENT_EXPIRATION = 105,
    SCARLET_WIDEVINE_EVENT_SESSION_CLOSED = 106,
};

/* Session and init data types follow the CDM ABI numbering. */
enum {
    SCARLET_WIDEVINE_SESSION_TEMPORARY = 0,
    SCARLET_WIDEVINE_SESSION_PERSISTENT_LICENSE = 1,
};

enum {
    SCARLET_WIDEVINE_INIT_DATA_CENC = 0,
    SCARLET_WIDEVINE_INIT_DATA_KEYIDS = 1,
    SCARLET_WIDEVINE_INIT_DATA_WEBM = 2,
};

/* Key statuses follow CDMKeyStatus numbering used by the CDM ABI. */
enum {
    SCARLET_WIDEVINE_KEY_USABLE = 0,
    SCARLET_WIDEVINE_KEY_INTERNAL_ERROR = 1,
    SCARLET_WIDEVINE_KEY_EXPIRED = 2,
    SCARLET_WIDEVINE_KEY_OUTPUT_RESTRICTED = 3,
    SCARLET_WIDEVINE_KEY_OUTPUT_DOWNSCALED = 4,
    SCARLET_WIDEVINE_KEY_STATUS_PENDING = 5,
    SCARLET_WIDEVINE_KEY_RELEASED = 6,
};

ScarletWidevineHandle* cdm_host_create(ScarletWidevineBytes host_path, ScarletWidevineBytes cdm_path, ScarletWidevineBytes ld_path, uint32_t slot_count, uint32_t slot_size);
void cdm_host_destroy(ScarletWidevineHandle*);
int32_t cdm_host_initialize(ScarletWidevineHandle*, uint32_t allow_distinctive_identifier, uint32_t allow_persistent_state, uint32_t use_hw_secure_codecs);
/* out_message_type receives the CDM message type of out_message. */
int32_t cdm_host_create_session(ScarletWidevineHandle*, ScarletWidevineBytes init_data, uint32_t session_type, uint32_t init_data_type, uint32_t* out_message_type, ScarletWidevineBuffer* out_session_id, ScarletWidevineBuffer* out_message);
/* out_keys is packed as count, then per key: size, key id, status. */
int32_t cdm_host_update_session(ScarletWidevineHandle*, ScarletWidevineBytes session_id, ScarletWidevineBytes response, ScarletWidevineBuffer* out_keys);
int32_t cdm_host_close_session(ScarletWidevineHandle*, ScarletWidevineBytes session_id);
/* subsamples is packed as count, then clear and cipher pairs. */
int32_t cdm_host_decrypt(ScarletWidevineHandle*, ScarletWidevineBytes sample, ScarletWidevineBytes key_id, ScarletWidevineBytes iv, ScarletWidevineBytes subsamples, uint32_t scheme, int64_t timestamp, ScarletWidevineBuffer* out_plaintext);
/* Returns 1 when out_event was filled, 0 when the queue is empty. The packed
 * event is kind, then the payloads documented in cdm_bridge.rs. */
int32_t cdm_host_take_event(ScarletWidevineHandle*, ScarletWidevineBuffer* out_event);
void cdm_host_buffer_free(ScarletWidevineBuffer);

#ifdef __cplusplus
}

namespace WebCore {
class Page;
void provideScarletMediaKeySystemTo(Page&);
}
#endif
