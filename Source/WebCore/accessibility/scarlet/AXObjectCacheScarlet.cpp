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

// The disabled profile never constructs an AXObjectCache. These ABI entries
// remain referenced by generic WebCore code; reaching an operation would mean
// that the disabled contract was bypassed, not that a native service ran.
void AXObjectCache::attachWrapper(AccessibilityObject&)
{
    RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Scarlet native accessibility wrapper attachment is unavailable");
}

void AXObjectCache::postPlatformNotification(AccessibilityObject&, AXNotification)
{
    RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Scarlet native accessibility notifications are unavailable");
}

void AXObjectCache::nodeTextChangePlatformNotification(AccessibilityObject*, AXTextChange, unsigned, const String&)
{
    RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Scarlet native accessibility text notifications are unavailable");
}

void AXObjectCache::frameLoadingEventPlatformNotification(RenderView*, AXLoadingEvent)
{
    RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Scarlet native accessibility loading notifications are unavailable");
}

void AXObjectCache::handleScrolledToAnchor(const Node&)
{
    RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Scarlet native accessibility anchor notifications are unavailable");
}

void AXObjectCache::platformHandleFocusedUIElementChanged(AccessibilityObject*, AccessibilityObject*)
{
    RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Scarlet native accessibility focus notifications are unavailable");
}

void AXObjectCache::platformPerformDeferredCacheUpdate()
{
    RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Scarlet native accessibility cache updates are unavailable");
}

} // namespace WebCore
