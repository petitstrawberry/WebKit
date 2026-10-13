/*
 * Copyright (C) 2012 Google Inc. All rights reserved.
 * Copyright (C) 2020 Sony Interactive Entertainment Inc. All Rights reserved.
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <cstddef>
#include <wtf/Assertions.h>

namespace WebCore {

class ScarletPlatformLayer;

// A CPU-only port has no platform compositor objects. Generic declarations need
// a nullable container type, but RefPtr<void*> cannot express that contract.
// This type can hold only the absence of a layer. It neither owns nor paints
// anything and rejects non-null inputs instead of dropping them silently.
class ScarletPlatformLayerContainer final {
public:
    constexpr ScarletPlatformLayerContainer() = default;
    constexpr ScarletPlatformLayerContainer(std::nullptr_t) { }
    explicit ScarletPlatformLayerContainer(ScarletPlatformLayer* layer)
    {
        RELEASE_ASSERT_WITH_MESSAGE(!layer, "Scarlet CPU WebCore cannot retain a platform compositor layer");
    }

    constexpr ScarletPlatformLayer* get() const { return nullptr; }
    constexpr explicit operator bool() const { return false; }
    constexpr bool operator!() const { return true; }
    constexpr bool operator==(const ScarletPlatformLayerContainer&) const = default;
};

} // namespace WebCore
