/*
 * Copyright (C) 2006 Eric Seidel (eric@webkit.org)
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012 Samsung Electronics. All rights reserved.
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

#include "config.h"
#include "ScarletChromeClient.h"

#include "ColorChooser.h"
#include "ColorChooserClient.h"
#include "DataListSuggestionPicker.h"
#include "DataListSuggestionsClient.h"
#include "DateTimeChooser.h"
#include "DateTimeChooserClient.h"
#include "FileChooser.h"
#include "FileIconLoader.h"
#include "FocusOptions.h"
#include "Icon.h"
#include "Page.h"
#include "PopupMenu.h"
#include "PopupMenuClient.h"
#include "RegistrableDomain.h"
#include "ScarletFrameLoaderClient.h"
#if USE(COORDINATED_GRAPHICS)
#include "ScarletLayerTree.h"
#endif
#include "SearchPopupMenu.h"
#include "TextIndicator.h"
#include <cstdio>
#include <cstdlib>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WeakPtr.h>
#include <wtf/text/CString.h>

namespace WebCore {

static void recordUnsupported(ScarletFrameLoadState& state, const String& message)
{
    // Preserve the first failure so an unsupported UI request cannot hide a
    // document-loading error or another rejected host operation.
    if (state.error.isEmpty())
        state.error = message;
}

// WebCore's select and search controls require a non-null popup object. This
// adapter closes a requested popup without changing the selected DOM value.
class ScarletCancelledPopupMenu final : public PopupMenu {
public:
    ScarletCancelledPopupMenu(PopupMenuClient& client, ScarletFrameLoadState& state)
        : m_client(client)
        , m_state(state)
    {
    }

private:
    void show(const IntRect&, LocalFrameView&, int) final
    {
        recordUnsupported(m_state, "Scarlet embedding does not provide native popup menus"_s);
        // The callback can destroy the control and disconnect this menu.
        RefPtr client = m_client.get();
        if (client)
            client->popupDidHide();
    }
    void hide() final
    {
        RefPtr client = m_client.get();
        if (client)
            client->popupDidHide();
    }
    // There is no open host menu to update after show() has cancelled it.
    void updateFromElement() final { }
    void disconnectClient() final { m_client = nullptr; }

    WeakPtr<PopupMenuClient> m_client;
    ScarletFrameLoadState& m_state;
};

class ScarletDisabledSearchPopupMenu final : public SearchPopupMenu {
public:
    ScarletDisabledSearchPopupMenu(PopupMenuClient& client, ScarletFrameLoadState& state)
        : m_popup(adoptRef(*new ScarletCancelledPopupMenu(client, state)))
        , m_state(state)
    {
    }

private:
    PopupMenu* popupMenu() final { return m_popup.ptr(); }
    bool enabled() final { return false; }
    void saveRecentSearches(const AtomString&, const Vector<RecentSearch>&) final
    {
        recordUnsupported(m_state, "Scarlet embedding does not persist recent searches"_s);
    }
    void loadRecentSearches(const AtomString&, Vector<RecentSearch>& searches) final { searches.clear(); }

    const Ref<ScarletCancelledPopupMenu> m_popup;
    ScarletFrameLoadState& m_state;
};

// This implements the native ChromeClient contract directly: EmptyChromeClient
// makes most overrides final and suppresses real repaint notifications.
class ScarletChromeClient final : public ChromeClient {
    WTF_MAKE_TZONE_ALLOCATED(ScarletChromeClient);
public:
#if USE(COORDINATED_GRAPHICS)
    ScarletChromeClient(ScarletFrameLoadState& state, ScarletLayerTree& layerTree)
        : m_state(state)
        , m_layerTree(layerTree)
    {
    }
#else
    explicit ScarletChromeClient(ScarletFrameLoadState& state)
        : m_state(state)
    {
    }
#endif

private:
    void repaint(const char* reason) const
    {
        if (m_tracePaintInvalidations)
            fprintf(stderr, "SCARLET_WEBCORE_PAINT_INVALIDATION reason=%s scope=full already_dirty=%u\n", reason, static_cast<unsigned>(m_state.needsPaint));
        m_state.needsPaint = true;
        m_state.fullPaint = true;
    }
    void repaint(const IntRect& rect, const char* reason) const
    {
        if (rect.isEmpty())
            return;
        if (m_tracePaintInvalidations)
            fprintf(stderr, "SCARLET_WEBCORE_PAINT_INVALIDATION reason=%s scope=rect x=%d y=%d width=%d height=%d already_dirty=%u\n", reason, rect.x(), rect.y(), rect.width(), rect.height(), static_cast<unsigned>(m_state.needsPaint));
        m_state.needsPaint = true;
        m_state.dirtyRect.unite(rect);
    }
    void unsupported(const String& message) const { recordUnsupported(m_state, message); }

    // Page owns the UniqueRef. The host viewport remains alive until Page has
    // been destroyed; ChromeClient does not own a second native window.
    void chromeDestroyed() final { }
    void setWindowRect(const FloatRect&) final { unsupported("Scarlet embedding requires host-controlled window sizing"_s); }
    FloatRect windowRect() const final { return m_state.viewport; }
    FloatRect pageRect() const final { return m_state.viewport; }

    // Native focus is supplied by the embedding. DOM focus and focus-ring
    // painting remain WebCore's responsibility.
    void focus() final { repaint(__func__); }
    void unfocus() final { repaint(__func__); }
    bool canTakeFocus(FocusDirection) const final { return false; }
    void takeFocus(FocusDirection) final { unsupported("Scarlet embedding cannot transfer focus to native chrome"_s); }
    void focusedElementChanged(Element*, LocalFrame*, FocusOptions, BroadcastFocusedElement) final { repaint(__func__); }
    void focusedFrameChanged(Frame*) final { repaint(__func__); }
    void elementDidFocus(Element&, const FocusOptions&) final { repaint(__func__); }
    void elementDidBlur(Element&) final { repaint(__func__); }
    void elementDidRefocus(Element&, const FocusOptions&) final { repaint(__func__); }

    RefPtr<Page> createWindow(LocalFrame&, const String&, const WindowFeatures&, const NavigationAction&) final
    {
        unsupported("Scarlet embedding does not create additional windows"_s);
        return nullptr;
    }
    void show() final { repaint(__func__); }
    bool canRunModal() const final { return false; }
    void runModal() final { unsupported("Scarlet embedding does not run modal windows"_s); }
    bool isPopup() const final { return false; }
    void setResizable(bool) final { unsupported("Scarlet embedding requires host-controlled window resizing"_s); }
    void closeWindow() final { unsupported("Scarlet embedding requires host-controlled window closing"_s); }

    void addMessageToConsole(JSC::MessageSource source, JSC::MessageLevel level, const String& message, unsigned line, unsigned column, const String& sourceID) final
    {
        auto messageUTF8 = message.utf8();
        auto sourceUTF8 = sourceID.utf8();
        std::fprintf(stderr, "SCARLET_WEBCORE_CONSOLE source=%u level=%u line=%u column=%u message=%s source_id=%s\n",
            static_cast<unsigned>(source), static_cast<unsigned>(level), line, column,
            reinterpret_cast<const char*>(messageUTF8.data()), reinterpret_cast<const char*>(sourceUTF8.data()));
    }

    bool canRunBeforeUnloadConfirmPanel() final { return false; }
    bool runBeforeUnloadConfirmPanel(String&&, LocalFrame&) final
    {
        unsupported("Scarlet embedding does not provide before-unload confirmation"_s);
        return false;
    }
    void runJavaScriptAlert(LocalFrame&, const String&) final { unsupported("Scarlet embedding does not provide JavaScript alert dialogs"_s); }
    bool runJavaScriptConfirm(LocalFrame&, const String&) final
    {
        unsupported("Scarlet embedding does not provide JavaScript confirmation dialogs"_s);
        return false;
    }
    bool runJavaScriptPrompt(LocalFrame&, const String&, const String&, String& result) final
    {
        unsupported("Scarlet embedding does not provide JavaScript prompt dialogs"_s);
        result = { };
        return false;
    }

    KeyboardUIMode keyboardUIMode() final { return static_cast<KeyboardUIMode>(KeyboardAccessFull | KeyboardAccessTabsToLinks); }
    bool hasAccessoryMousePointingDevice() const final { return false; }
    bool hoverSupportedByPrimaryPointingDevice() const final { return true; }
    bool hoverSupportedByAnyAvailablePointingDevice() const final { return true; }
    std::optional<PointerCharacteristics> pointerCharacteristicsOfPrimaryPointingDevice() const final { return PointerCharacteristics::Fine; }
    OptionSet<PointerCharacteristics> pointerCharacteristicsOfAllAvailablePointingDevices() const final { return PointerCharacteristics::Fine; }

    void invalidateRootView(const IntRect& rect) final { repaint(rect, __func__); }
    void invalidateContentsAndRootView(const IntRect& rect) final { repaint(rect, __func__); }
    void invalidateContentsForSlowScroll(const IntRect& rect) final { repaint(rect, __func__); }
    // No native scroll blit is attempted: the host repaints the actual WebCore
    // view at its updated scroll position on the next tick.
    void scroll(const IntSize&, const IntRect&, const IntRect&) final { repaint(__func__); }
    void triggerRenderingUpdate() final { m_state.needsRenderingUpdate = true; }
    bool scheduleRenderingUpdate() final { m_state.needsRenderingUpdate = true; return true; }
    void setNeedsOneShotDrawingSynchronization() final { repaint(__func__); }
    void renderingUpdateFramesPerSecondChanged() final { m_state.needsRenderingUpdate = true; }
    void frameViewLayoutOrVisualViewportChanged(const LocalFrameView&) final
    {
        // This is a post-layout observation, also sent for an unchanged
        // viewport. It is not a paint invalidation. Host resize explicitly
        // dirties the view; WebCore scroll and renderer invalidations reach
        // scroll(), invalidateContentsAndRootView() and the slow-scroll path.
    }

    // ABI coordinates are native CSS pixels in the embedding's viewport.
    IntPoint screenToRootView(const IntPoint& point) const final { return point; }
    IntPoint rootViewToScreen(const IntPoint& point) const final { return point; }
    IntRect rootViewToScreen(const IntRect& rect) const final { return rect; }
    IntPoint accessibilityScreenToRootView(const IntPoint& point) const final { return point; }
    IntRect rootViewToAccessibilityScreen(const IntRect& rect) const final { return rect; }

    PlatformPageClient platformPageClient() const final { return nullptr; }
    void rootFrameAdded(const LocalFrame&) final { repaint(__func__); }
    void rootFrameRemoved(const LocalFrame&) final { repaint(__func__); }
    void mainFrameDidChange() final { repaint(__func__); }
    void didFinishLoadingImageForElement(HTMLImageElement&) final
    {
        // Page delivers this UI/image-analysis observation in a queued task.
        // RenderImage::imageChanged has already invalidated the decoded pixels
        // or layout through the renderer's normal repaint path. The embedding
        // has no image-analysis delegate; this notification adds no damage.
    }
    void didFinishLoadingImageForSVGImage(SVGImageElement&) final { repaint(__func__); }
    void contentsSizeChanged(LocalFrame&, const IntSize&) const final { repaint(__func__); }
    void intrinsicContentsSizeChanged(const IntSize&) const final { repaint(__func__); }
    void scrollOriginDidChange(const LocalFrame&) const final { repaint(__func__); }

    // ABI v1 has no native tooltip or cursor-shape channel. Hover hit testing,
    // CSS state and mouse input are still performed by WebCore.
    void mouseDidMoveOverElement(const HitTestResult&, OptionSet<PlatformEventModifier>, const String&, TextDirection) final { }
    bool supportsSettingCursor() final { return false; }
    void setCursor(const Cursor&) final { }
    void setCursorHiddenUntilMouseMoves(bool) final { }
    void wheelEventHandlersChanged(bool) final { }

    void print(LocalFrame&, const StringWithDirection&) final { unsupported("Scarlet embedding does not provide printing"_s); }
    void exceededDatabaseQuota(LocalFrame&, const String&, DatabaseDetails) final { unsupported("Scarlet embedding does not increase database quota"_s); }
    RefPtr<ColorChooser> createColorChooser(ColorChooserClient& client, const Color&) final
    {
        unsupported("Scarlet embedding does not provide a native color chooser"_s);
        client.didEndChooser();
        return nullptr;
    }
    RefPtr<DataListSuggestionPicker> createDataListSuggestionPicker(DataListSuggestionsClient& client) final
    {
        unsupported("Scarlet embedding does not provide a native suggestion picker"_s);
        client.didCloseSuggestions();
        return nullptr;
    }
    bool canShowDataListSuggestionLabels() const final { return false; }
    RefPtr<DateTimeChooser> createDateTimeChooser(DateTimeChooserClient& client) final
    {
        unsupported("Scarlet embedding does not provide a native date/time chooser"_s);
        client.didEndChooser();
        return nullptr;
    }
    RefPtr<PopupMenu> createPopupMenu(PopupMenuClient& client) const final
    {
        return adoptRef(*new ScarletCancelledPopupMenu(client, m_state));
    }
    RefPtr<SearchPopupMenu> createSearchPopupMenu(PopupMenuClient& client) const final
    {
        return adoptRef(*new ScarletDisabledSearchPopupMenu(client, m_state));
    }

    void setTextIndicator(RefPtr<TextIndicator>&& indicator) const final
    {
        if (indicator)
            unsupported("Scarlet embedding does not display native text indicators"_s);
    }
    void updateTextIndicator(RefPtr<TextIndicator>&& indicator) const final
    {
        if (indicator)
            unsupported("Scarlet embedding does not display native text indicators"_s);
    }
    // A null monitor factory keeps the real WebCore timer fallback available;
    // rendering itself is driven by the host's Page::updateRendering() ticks.
    DisplayRefreshMonitorFactory* displayRefreshMonitorFactory() const final { return nullptr; }
    void runOpenPanel(LocalFrame&, FileChooser& chooser) final
    {
        unsupported("Scarlet embedding does not provide a native file chooser"_s);
        chooser.cancelFileChoosing();
    }
    void transcodeChosenFiles(Vector<String>&&, String&&, String&&, CompletionHandler<void(Vector<String>&&)>&& completion) final
    {
        unsupported("Scarlet embedding does not transcode selected files"_s);
        completion({ });
    }
    void showShareSheet(ShareDataWithParsedURL&&, CompletionHandler<void(bool)>&& completion) final
    {
        unsupported("Scarlet embedding does not provide a native share sheet"_s);
        completion(false);
    }
    void showContactPicker(ContactsRequestData&&, CompletionHandler<void(std::optional<Vector<ContactInfo>>&&)>&& completion) final
    {
        unsupported("Scarlet embedding does not provide a native contact picker"_s);
        completion(std::nullopt);
    }
    void loadIconForFiles(const Vector<String>&, FileIconLoader& loader) final
    {
        unsupported("Scarlet embedding does not provide native file icons"_s);
        loader.iconLoaded(nullptr);
    }
    RefPtr<Icon> createIconForFiles(const Vector<String>&) final
    {
        unsupported("Scarlet embedding does not provide native file icons"_s);
        return nullptr;
    }

#if USE(COORDINATED_GRAPHICS)
    bool allowsAcceleratedCompositing() const final { return true; }
    CompositingTriggerFlags allowedCompositingTriggers() const final { return static_cast<CompositingTriggerFlags>(AllTriggers); }
    GraphicsLayerFactory* graphicsLayerFactory() const final { return m_layerTree.factory(); }
    void attachRootGraphicsLayer(LocalFrame&, GraphicsLayer* layer) final { m_layerTree.setRootLayer(layer); }
    void attachViewOverlayGraphicsLayer(GraphicsLayer* layer) final { m_layerTree.setOverlayLayer(layer); }
#else
    bool allowsAcceleratedCompositing() const final { return false; }
    CompositingTriggerFlags allowedCompositingTriggers() const final { return 0; }
    void attachRootGraphicsLayer(LocalFrame&, GraphicsLayer* layer) final
    {
        if (layer)
            unsupported("Scarlet embedding paints without a native layer compositor"_s);
    }
    void attachViewOverlayGraphicsLayer(GraphicsLayer* layer) final
    {
        if (layer)
            unsupported("Scarlet embedding does not attach native overlay layers"_s);
    }
#endif
    bool usePluginRendererScrollableArea(LocalFrame&) const final { return false; }
    void exitVideoFullscreenForVideoElement(HTMLVideoElement&, CompletionHandler<void(bool)>&& completion) final
    {
        unsupported("Scarlet embedding does not provide video fullscreen UI"_s);
        completion(false);
    }

    void setLoginStatus(RegistrableDomain&&, IsLoggedIn, CompletionHandler<void()>&& completion) final
    {
        unsupported("Scarlet embedding does not store native login status"_s);
        completion();
    }
    void isLoggedIn(RegistrableDomain&&, CompletionHandler<void(bool)>&& completion) final
    {
        unsupported("Scarlet embedding does not store native login status"_s);
        completion(false);
    }
    void registerBlobPathForTesting(const String&, CompletionHandler<void()>&& completion) final
    {
        unsupported("Scarlet embedding does not register testing blob paths"_s);
        completion();
    }

    bool isEmptyChromeClient() const final { return false; }

    ScarletFrameLoadState& m_state;
#if USE(COORDINATED_GRAPHICS)
    ScarletLayerTree& m_layerTree;
#endif
    const bool m_tracePaintInvalidations { std::getenv("MYRICA_WEBKIT_TRACE_PAINT") != nullptr };
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(ScarletChromeClient);

#if USE(COORDINATED_GRAPHICS)
UniqueRef<ChromeClient> createScarletChromeClient(ScarletFrameLoadState& state, ScarletLayerTree& layerTree)
{
    return makeUniqueRef<ScarletChromeClient>(state, layerTree);
}
#else
UniqueRef<ChromeClient> createScarletChromeClient(ScarletFrameLoadState& state)
{
    return makeUniqueRef<ScarletChromeClient>(state);
}
#endif

} // namespace WebCore
