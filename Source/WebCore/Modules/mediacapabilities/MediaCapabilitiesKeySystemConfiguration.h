/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once
#if ENABLE(ENCRYPTED_MEDIA)
#include "MediaCapabilitiesKeySystemMediaCapability.h"
#include "MediaKeysRequirement.h"
#include "MediaKeySessionType.h"
#include <wtf/Vector.h>
namespace WebCore {
struct MediaCapabilitiesKeySystemConfiguration {
    String keySystem;
    String initDataType;
    CDMRequirement distinctiveIdentifier { CDMRequirement::Optional };
    CDMRequirement persistentState { CDMRequirement::Optional };
    Vector<CDMSessionType> sessionTypes;
    std::optional<MediaCapabilitiesKeySystemMediaCapability> audio;
    std::optional<MediaCapabilitiesKeySystemMediaCapability> video;
    MediaCapabilitiesKeySystemConfiguration isolatedCopy() const
    {
        return { keySystem.isolatedCopy(), initDataType.isolatedCopy(), distinctiveIdentifier, persistentState, sessionTypes,
            audio ? std::optional { audio->isolatedCopy() } : std::nullopt,
            video ? std::optional { video->isolatedCopy() } : std::nullopt };
    }
};
}
#endif
