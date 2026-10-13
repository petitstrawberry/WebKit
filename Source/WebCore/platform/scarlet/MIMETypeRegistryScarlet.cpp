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
#include "MIMETypeRegistry.h"

namespace WebCore {

// This table describes file formats, not decoder or URL-loader availability.
static constexpr TypeExtensionPair mediaTypes[] = {
    { "text/html"_s, "html"_s }, { "text/html"_s, "htm"_s },
    { "text/css"_s, "css"_s }, { "text/plain"_s, "txt"_s }, { "text/plain"_s, "text"_s },
    { "text/javascript"_s, "js"_s }, { "text/javascript"_s, "mjs"_s },
    { "application/json"_s, "json"_s }, { "application/xml"_s, "xml"_s },
    { "application/xhtml+xml"_s, "xhtml"_s }, { "image/svg+xml"_s, "svg"_s },
    { "image/png"_s, "png"_s }, { "image/jpeg"_s, "jpg"_s }, { "image/jpeg"_s, "jpeg"_s },
    { "image/gif"_s, "gif"_s }, { "image/webp"_s, "webp"_s },
    { "image/bmp"_s, "bmp"_s }, { "image/x-icon"_s, "ico"_s },
    { "font/ttf"_s, "ttf"_s }, { "font/otf"_s, "otf"_s },
    { "font/woff"_s, "woff"_s }, { "font/woff2"_s, "woff2"_s },
    { "application/pdf"_s, "pdf"_s },
};

String MIMETypeRegistry::mimeTypeForExtension(StringView extension)
{
    for (auto& entry : mediaTypes) {
        if (equalIgnoringASCIICase(extension, entry.extension))
            return entry.type;
    }
    return emptyString();
}

String MIMETypeRegistry::preferredExtensionForMIMEType(const String& type)
{
    for (auto& entry : mediaTypes) {
        if (equalIgnoringASCIICase(type, entry.type))
            return entry.extension;
    }
    return emptyString();
}

Vector<String> MIMETypeRegistry::extensionsForMIMEType(const String& type)
{
    Vector<String> result;
    for (auto& entry : mediaTypes) {
        if (equalIgnoringASCIICase(type, entry.type))
            result.append(entry.extension);
    }
    return result;
}

bool MIMETypeRegistry::isApplicationPluginMIMEType(const String&) { return false; }

} // namespace WebCore
