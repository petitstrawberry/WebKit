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
#include "PlatformScreen.h"
#include "ScarletPlatform.h"
#include "ColorSpace.h"
#include "ContentsFormat.h"
#include <cmath>
#include <wtf/Assertions.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>

namespace WebCore {

struct ScarletScreenConfiguration {
    FloatRect display;
    FloatRect available;
};

static std::optional<ScarletScreenConfiguration>& screenConfiguration()
{
    static NeverDestroyed<std::optional<ScarletScreenConfiguration>> configuration;
    return configuration.get();
}

void setScarletScreenConfiguration(const FloatRect& display, const FloatRect& available)
{
    RELEASE_ASSERT(isMainThread());
    auto valid = [](const FloatRect& rect) {
        return std::isfinite(rect.x()) && std::isfinite(rect.y())
            && std::isfinite(rect.width()) && std::isfinite(rect.height())
            && rect.width() > 0 && rect.height() > 0;
    };
    RELEASE_ASSERT_WITH_MESSAGE(valid(display) && valid(available) && display.contains(available),
        "Scarlet WebCore requires finite configured display bounds");
    screenConfiguration() = ScarletScreenConfiguration { display, available };
}

static const ScarletScreenConfiguration& configuration()
{
    RELEASE_ASSERT(isMainThread());
    RELEASE_ASSERT_WITH_MESSAGE(screenConfiguration().has_value(),
        "Call setScarletScreenConfiguration before creating a Scarlet WebCore Page");
    return *screenConfiguration();
}

// The CPU output contract is 8-bit sRGB BGRA. Alpha is not screen color depth.
int screenDepth(Widget*) { return 24; }
int screenDepthPerComponent(Widget*) { return 8; }
bool screenIsMonochrome(Widget*) { return false; }
bool screenHasInvertedColors() { return false; }
FloatRect screenRect(Widget*) { return configuration().display; }
FloatRect screenAvailableRect(Widget*) { return configuration().available; }
bool screenSupportsExtendedColor(Widget*) { return false; }
ColorSpace screenColorSpace(Widget*) { return ColorSpace::SRGB(); }
OptionSet<ContentsFormat> screenContentsFormats(Widget*) { return { ContentsFormat::RGBA8 }; }

} // namespace WebCore
