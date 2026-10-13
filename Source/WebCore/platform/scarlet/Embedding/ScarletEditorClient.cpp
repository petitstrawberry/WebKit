/*
 * Copyright (C) 2006 Eric Seidel <eric@webkit.org>
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
 * Copyright (C) Research In Motion Limited 2011. All rights reserved.
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
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ScarletEditorClient.h"

#include "DOMPasteAccess.h"
#include "Document.h"
#include "Editor.h"
#include "EventNames.h"
#include "FrameDestructionObserverInlines.h"
#include "KeyboardEvent.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "Node.h"
#include "NodeDocument.h"
#include "PlatformKeyboardEvent.h"
#include "TextCheckerClient.h"
#include "WindowsKeyboardCodes.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

namespace WebCore {

// The embedding has no spelling service. Queries stay disabled, synchronous
// outputs are initialized, and asynchronous requests receive cancellation.
class ScarletDisabledTextChecker final : public TextCheckerClient {
    bool shouldEraseMarkersAfterChangeSelection(TextCheckingType) const final { return true; }
    void ignoreWordInSpellDocument(const String&) final { }
    void learnWord(const String&) final { }
    void checkSpellingOfString(StringView, int* location, int* length) final
    {
        if (location)
            *location = -1;
        if (length)
            *length = 0;
    }
    void checkGrammarOfString(StringView, Vector<GrammarDetail>& details, int* location, int* length) final
    {
        details.clear();
        if (location)
            *location = -1;
        if (length)
            *length = 0;
    }
#if USE(UNIFIED_TEXT_CHECKING)
    Vector<TextCheckingResult> checkTextOfParagraph(StringView, OptionSet<TextCheckingType>, const VisibleSelection&) final { return { }; }
#endif
    void getGuessesForWord(const String&, const String&, const VisibleSelection&, Vector<String>& guesses) final { guesses.clear(); }
    void requestCheckingOfString(TextCheckingRequest& request, const VisibleSelection&) final { request.didCancel(); }
    void requestExtendedCheckingOfString(TextCheckingRequest& request, const VisibleSelection&) final { request.didCancel(); }
};

class ScarletEditorClient final : public EditorClient {
    WTF_MAKE_TZONE_ALLOCATED(ScarletEditorClient);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ScarletEditorClient);
public:
    ScarletEditorClient() = default;

private:
    bool shouldDeleteRange(const std::optional<SimpleRange>&) final { return true; }
    bool smartInsertDeleteEnabled() final { return false; }
    bool isSelectTrailingWhitespaceEnabled() const final { return false; }
    bool isContinuousSpellCheckingEnabled() final { return false; }
    void toggleContinuousSpellChecking() final { }
    bool isGrammarCheckingEnabled() final { return false; }
    void toggleGrammarChecking() final { }
    int spellCheckerDocumentTag() final { return -1; }

    bool shouldBeginEditing(const SimpleRange&) final { return true; }
    bool shouldEndEditing(const SimpleRange&) final { return true; }
    bool shouldInsertNode(Node&, const std::optional<SimpleRange>&, EditorInsertAction) final { return true; }
    bool shouldInsertText(const String&, const std::optional<SimpleRange>&, EditorInsertAction) final { return true; }
    bool shouldChangeSelectedRange(const std::optional<SimpleRange>&, const std::optional<SimpleRange>&, Affinity, bool) final { return true; }
    bool shouldApplyStyle(const StyleProperties&, const std::optional<SimpleRange>&) final { return true; }
    void didApplyStyle() final { }
    bool shouldMoveRangeAfterDelete(const SimpleRange&, const SimpleRange&) final { return true; }

    // These are notifications, not requests to perform editing. WebCore owns
    // the DOM mutation, selection and layout; there is no separate OS editor UI.
    void didBeginEditing() final { }
    void respondToChangedContents() final { }
    void respondToChangedSelection(LocalFrame*) final { }
    void didEndUserTriggeredSelectionChanges() final { }
    void updateEditorStateAfterLayoutIfEditabilityChanged() final { }
    void didEndEditing() final { }
    void willWriteSelectionToPasteboard(const std::optional<SimpleRange>&) final { }
    void didWriteSelectionToPasteboard() final { }
    void getClientPasteboardData(const std::optional<SimpleRange>&, Vector<std::pair<String, RefPtr<SharedBuffer>>>& data) final { data.clear(); }
    DOMPasteAccessResponse requestDOMPasteAccess(DOMPasteAccessCategory, FrameIdentifier, const String&) final { return DOMPasteAccessResponse::DeniedForGesture; }

    // Clipboard, native IME and undo history are not exposed by ABI v1.
    bool canCopyCut(LocalFrame*, bool) const final { return false; }
    bool canPaste(LocalFrame*, bool) const final { return false; }
    bool canUndo() const final { return false; }
    bool canRedo() const final { return false; }
    void registerUndoStep(UndoStep& step) final { step.didRemoveFromUndoManager(); }
    void registerRedoStep(UndoStep& step) final { step.didRemoveFromUndoManager(); }
    void clearUndoRedoOperations() final { }
    void undo() final { }
    void redo() final { }

    void handleKeyboardEvent(KeyboardEvent&) final;
    // Leave pre-DOM keydown unhandled. Ordinary editing runs only after the DOM
    // has had the opportunity to cancel it; no IME composition is synthesized.
    void handleInputMethodKeydown(KeyboardEvent&) final { }
    void discardedComposition(const Document&) final { }
    void canceledComposition() final { }
    void didUpdateComposition() final { }
    void setInputMethodState(Element*) final { }

    void textFieldDidBeginEditing(Element&) final { }
    void textFieldDidEndEditing(Element&) final { }
    void textDidChangeInTextField(Element&) final { }
    bool doTextFieldCommandFromEvent(Element&, KeyboardEvent*) final { return false; }
    void textWillBeDeletedInTextField(Element&) final { }
    void textDidChangeInTextArea(Element&) final { }
    void overflowScrollPositionChanged() final { }
    void subFrameScrollPositionChanged() final { }
    bool performTwoStepDrop(DocumentFragment&, const SimpleRange&, bool) final { return false; }

    TextCheckerClient* textChecker() final { return &m_textChecker; }
    void updateSpellingUIWithGrammarString(const String&, const GrammarDetail&) final { }
    void updateSpellingUIWithMisspelledWord(const String&) final { }
    void showSpellingUI(bool) final { }
    bool spellingUIIsShowing() final { return false; }

#if PLATFORM(COCOA)
    HashMap<FrameIdentifier, AttributedString> collectAttributedStringsForRemoteFrames(FrameIdentifier, const Vector<FrameIdentifier>&) final { return { }; }
    void setInsertionPasteboard(const String&) final { }
#endif
#if PLATFORM(IOS_FAMILY)
    void startDelayingAndCoalescingContentChangeNotifications() final { }
    void stopDelayingAndCoalescingContentChangeNotifications() final { }
    bool hasRichlyEditableSelection() final { return false; }
    int getPasteboardItemsCount() final { return 0; }
    RefPtr<DocumentFragment> documentFragmentFromDelegate(int) final { return nullptr; }
    bool performsTwoStepPaste(DocumentFragment*) final { return false; }
    void updateStringForFind(const String&) final { }
#endif
#if USE(APPKIT)
    void uppercaseWord() final { }
    void lowercaseWord() final { }
    void capitalizeWord() final { }
    bool canApplyCaseTransformations(const String&) final { return false; }
    bool canConvertToTraditionalChinese(const String&) final { return false; }
    bool canConvertToSimplifiedChinese(const String&) final { return false; }
    void convertToTraditionalChinese() final { }
    void convertToSimplifiedChinese() final { }
#endif
#if USE(AUTOMATIC_TEXT_REPLACEMENT)
    void showSubstitutionsPanel(bool) final { }
    bool substitutionsPanelIsShowing() final { return false; }
    void toggleSmartInsertDelete() final { }
    bool isAutomaticQuoteSubstitutionEnabled() final { return false; }
    void toggleAutomaticQuoteSubstitution() final { }
    bool isAutomaticLinkDetectionEnabled() final { return false; }
    void toggleAutomaticLinkDetection() final { }
    bool isAutomaticDashSubstitutionEnabled() final { return false; }
    void toggleAutomaticDashSubstitution() final { }
    bool isAutomaticTextReplacementEnabled() final { return false; }
    void toggleAutomaticTextReplacement() final { }
    bool isAutomaticSpellingCorrectionEnabled() final { return false; }
    void toggleAutomaticSpellingCorrection() final { }
    bool isSmartListsEnabled() final { return false; }
    void toggleSmartLists() final { }
#endif
#if PLATFORM(GTK)
    bool shouldShowUnicodeMenu() final { return false; }
#endif

    ScarletDisabledTextChecker m_textChecker;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(ScarletEditorClient);

static String keydownCommand(const PlatformKeyboardEvent& event)
{
    // ABI v1 has no AltGr or platform-specific Meta/Alt key bindings.
    if (event.altKey() || event.metaKey())
        return { };

    bool control = event.controlKey();
    bool shift = event.shiftKey();
    String movement;
    switch (event.windowsVirtualKeyCode()) {
    case VK_BACK:
        return control ? "DeleteWordBackward"_s : "DeleteBackward"_s;
    case VK_DELETE:
        // Shift+Delete is a clipboard shortcut, which this client does not offer.
        if (shift && !control)
            return { };
        return control ? "DeleteWordForward"_s : "DeleteForward"_s;
    case VK_LEFT:
        movement = control ? "MoveWordLeft"_s : "MoveLeft"_s;
        break;
    case VK_RIGHT:
        movement = control ? "MoveWordRight"_s : "MoveRight"_s;
        break;
    case VK_UP:
        movement = control ? "MoveToBeginningOfParagraph"_s : "MoveUp"_s;
        break;
    case VK_DOWN:
        movement = control ? "MoveToEndOfParagraph"_s : "MoveDown"_s;
        break;
    case VK_HOME:
        movement = control ? "MoveToBeginningOfDocument"_s : "MoveToBeginningOfLine"_s;
        break;
    case VK_END:
        movement = control ? "MoveToEndOfDocument"_s : "MoveToEndOfLine"_s;
        break;
    case VK_PRIOR:
        if (control)
            return { };
        movement = "MovePageUp"_s;
        break;
    case VK_NEXT:
        if (control)
            return { };
        movement = "MovePageDown"_s;
        break;
    case VK_RETURN:
        if (control)
            return { };
        return shift ? "InsertLineBreak"_s : "InsertNewline"_s;
    case VK_TAB:
        if (control)
            return { };
        return shift ? "InsertBacktab"_s : "InsertTab"_s;
    case VK_A:
        return control && !shift ? "SelectAll"_s : String { };
    case VK_B:
        return control && !shift ? "ToggleBold"_s : String { };
    case VK_I:
        return control && !shift ? "ToggleItalic"_s : String { };
    case VK_U:
        return control && !shift ? "ToggleUnderline"_s : String { };
    default:
        return { };
    }
    return shift ? makeString(movement, "AndModifySelection"_s) : movement;
}

static String nonEditableScrollCommand(const PlatformKeyboardEvent& event)
{
    if (event.altKey() || event.metaKey() || event.shiftKey())
        return { };
    switch (event.windowsVirtualKeyCode()) {
    case VK_HOME:
        return "ScrollToBeginningOfDocument"_s;
    case VK_END:
        return "ScrollToEndOfDocument"_s;
    case VK_PRIOR:
        return event.controlKey() ? String { } : "ScrollPageBackward"_s;
    case VK_NEXT:
        return event.controlKey() ? String { } : "ScrollPageForward"_s;
    default:
        return { };
    }
}

void ScarletEditorClient::handleKeyboardEvent(KeyboardEvent& event)
{
    if (event.defaultPrevented() || event.defaultHandled() || event.isDefaultEventHandlerIgnored() || event.isComposing())
        return;
    auto* platformEvent = event.underlyingPlatformEvent();
    if (!platformEvent || platformEvent->isSystemKey() || platformEvent->type() == PlatformEvent::Type::KeyUp)
        return;
    RefPtr target = dynamicDowncast<Node>(event.target());
    if (!target)
        return;
    RefPtr frame = target->document().frame();
    if (!frame)
        return;

    auto& editor = frame->editor();
    if (event.type() == eventNames().keydownEvent) {
        auto commandName = keydownCommand(*platformEvent);
        if (!commandName.isEmpty()) {
            auto command = editor.command(commandName);
            // Text insertion requires keypress. In particular, leaving Tab
            // unhandled here preserves WebCore's ordinary focus traversal.
            if (command.isTextInsertion())
                return;
            if (command.execute(&event)) {
                event.setDefaultHandled();
                return;
            }
        }
        if (!editor.canEdit()) {
            auto scrollCommand = nonEditableScrollCommand(*platformEvent);
            if (!scrollCommand.isEmpty() && editor.command(scrollCommand).execute(&event))
                event.setDefaultHandled();
        }
        return;
    }

    if (event.type() != eventNames().keypressEvent || platformEvent->type() != PlatformEvent::Type::Char)
        return;
    if (event.ctrlKey() || event.metaKey() || event.altKey())
        return;

    auto text = platformEvent->text();
    if (text.isEmpty())
        return;
    String insertionCommand;
    if (text.length() == 1) {
        switch (event.charCode()) {
        case '\r':
        case '\n':
            insertionCommand = event.shiftKey() ? "InsertLineBreak"_s : "InsertNewline"_s;
            break;
        case '\t':
            insertionCommand = event.shiftKey() ? "InsertBacktab"_s : "InsertTab"_s;
            break;
        }
    }
    if (!insertionCommand.isEmpty()) {
        if (editor.command(insertionCommand).execute(&event))
            event.setDefaultHandled();
        return;
    }

    if (!editor.canEdit())
        return;
    // Char is committed Unicode, not a platform virtual-key encoding. Preserve
    // every code unit, including private-use text and multi-line commits, and
    // let WebCore's real TextEvent/TypingCommand determine insertion behavior.
    if (editor.insertText(text, &event))
        event.setDefaultHandled();
}

UniqueRef<EditorClient> createScarletEditorClient()
{
    return makeUniqueRef<ScarletEditorClient>();
}

} // namespace WebCore
