/*
 * Copyright (C) 2023 Sony Interactive Entertainment Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

// Native accessibility is not part of the single-frame offline profile. Do not
// advertise an accessibility platform with successful no-op notifications.
#if ENABLE(ACCESSIBILITY)
#error "Scarlet CPU WebCore requires ENABLE_ACCESSIBILITY=OFF until a native accessibility adapter exists"
#endif

#include "AXObjectCache.h"
#include "AccessibilityObject.h"
#include <wtf/Assertions.h>

namespace WebCore {

void AccessibilityObject::detachPlatformWrapper(AccessibilityDetachmentType)
{
    // A disabled object has no native wrapper to detach. Validate the real
    // absent state; this performs no native service or notification.
    RELEASE_ASSERT_WITH_MESSAGE(AXObjectCache::accessibilityMode() == AccessibilityMode::Off,
        "Scarlet native accessibility must remain disabled");
    RELEASE_ASSERT_WITH_MESSAGE(!AXObjectCache::accessibilityEnabled(),
        "Scarlet native accessibility must remain disabled");
    RELEASE_ASSERT_WITH_MESSAGE(!wrapper(), "Scarlet cannot detach a native accessibility wrapper");
}

bool AccessibilityObject::accessibilityIgnoreAttachment() const
{
    // No concrete AX tree may be built in this profile. Do not synthesize a
    // platform inclusion decision for an object that should not exist.
    RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Scarlet native accessibility attachment queries are unavailable");
}

AccessibilityObjectInclusion AccessibilityObject::accessibilityPlatformIncludesObject() const
{
    RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Scarlet native accessibility inclusion queries are unavailable");
}

} // namespace WebCore
