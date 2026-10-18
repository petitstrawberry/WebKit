/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once
#if ENABLE(ENCRYPTED_MEDIA)
#include "MediaKeyEncryptionScheme.h"
#include <wtf/text/WTFString.h>
namespace WebCore {
struct MediaCapabilitiesKeySystemMediaCapability {
    String robustness;
    std::optional<CDMEncryptionScheme> encryptionScheme;
    MediaCapabilitiesKeySystemMediaCapability isolatedCopy() const { return { robustness.isolatedCopy(), encryptionScheme }; }
};
}
#endif
