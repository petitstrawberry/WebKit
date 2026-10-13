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
#include "Pasteboard.h"
#include <wtf/Assertions.h>

namespace WebCore {

// System clipboard integration is outside the offline renderer. Queries expose
// no system data; operations whose void result could imply a successful copy
// fail loudly instead of silently losing the user's data.
static void unavailableRead()
{
    WTFLogAlways("Scarlet CPU WebCore: system pasteboard is unavailable");
}

[[noreturn]] static void unavailableWrite()
{
    RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("Scarlet CPU WebCore: system pasteboard writes are unsupported");
}

std::unique_ptr<Pasteboard> Pasteboard::createForCopyAndPaste(std::unique_ptr<PasteboardContext>&& context)
{
    return makeUnique<Pasteboard>(WTF::move(context));
}
Pasteboard::Pasteboard(std::unique_ptr<PasteboardContext>&& context) : m_context(WTF::move(context)) { }
bool Pasteboard::hasData() { return false; }
Vector<String> Pasteboard::typesSafeForBindings(const String&) { return { }; }
Vector<String> Pasteboard::typesForLegacyUnsafeBindings() { return { }; }
String Pasteboard::readOrigin() { unavailableRead(); return { }; }
String Pasteboard::readString(const String&) { unavailableRead(); return { }; }
String Pasteboard::readStringInCustomData(const String&) { unavailableRead(); return { }; }
void Pasteboard::read(PasteboardPlainText& text, PlainTextURLReadingPolicy, std::optional<size_t>) { unavailableRead(); text.text = { }; }
void Pasteboard::read(PasteboardWebContentReader&, WebContentReadingPolicy, std::optional<size_t>) { unavailableRead(); }
void Pasteboard::read(PasteboardFileReader&, std::optional<size_t>) { unavailableRead(); }
Pasteboard::FileContentState Pasteboard::fileContentState() { return FileContentState::NoFileOrImageData; }
bool Pasteboard::canSmartReplace() { return false; }
void Pasteboard::writeString(const String&, const String&) { unavailableWrite(); }
void Pasteboard::clear() { unavailableWrite(); }
void Pasteboard::clear(const String&) { unavailableWrite(); }
void Pasteboard::write(const Color&) { unavailableWrite(); }
void Pasteboard::write(const PasteboardURL&) { unavailableWrite(); }
void Pasteboard::writeTrustworthyWebURLsPboardType(const PasteboardURL&) { unavailableWrite(); }
void Pasteboard::write(const PasteboardImage&) { unavailableWrite(); }
void Pasteboard::write(const PasteboardBuffer&) { unavailableWrite(); }
void Pasteboard::write(const PasteboardWebContent&) { unavailableWrite(); }
void Pasteboard::writeCustomData(const Vector<PasteboardCustomData>&, PasteboardWriteType) { unavailableWrite(); }
void Pasteboard::writeMarkup(const String&) { unavailableWrite(); }
void Pasteboard::writePlainText(const String&, SmartReplaceOption) { unavailableWrite(); }

#if ENABLE(DRAG_SUPPORT)
#error "Scarlet CPU WebCore does not implement native drag-and-drop"
#endif

} // namespace WebCore
