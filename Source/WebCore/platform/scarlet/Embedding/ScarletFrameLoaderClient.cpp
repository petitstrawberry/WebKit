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
#include "ScarletFrameLoaderClient.h"
#include "Color.h"
#include "BackForwardController.h"
#include "HistoryController.h"
#include "NavigationScheduler.h"
#include "Page.h"
#include "DocumentLoader.h"
#include "DocumentPage.h"
#include "DocumentView.h"
#include "FormState.h"
#include "FrameLoader.h"
#include "FrameDestructionObserverInlines.h"
#include "NodeInlines.h"
#include "FrameTree.h"
#include "FrameTreeSyncData.h"
#include "ReferrerPolicy.h"
#include "FrameNetworkingContext.h"
#include "HTMLFrameOwnerElement.h"
#include "HistoryItem.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "LocalFrameView.h"
#include "MIMETypeRegistry.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "ScrollbarMode.h"
#include "SharedBuffer.h"
#include "SubstituteData.h"
#include "Widget.h"
#include <cstdlib>
#include <wtf/text/AtomString.h>

namespace WebCore {
using namespace WTF::StringLiterals;
class ScarletFrameNetworkingContext final : public FrameNetworkingContext {
public:
    static Ref<ScarletFrameNetworkingContext> create(LocalFrame& frame) { return adoptRef(*new ScarletFrameNetworkingContext(frame)); }
private:
    ScarletFrameNetworkingContext(LocalFrame&);
    CookieStorageSession* storageSession() const final { return nullptr; }
};

void configureScarletFrameView(LocalFrameView& view)
{
    // Native wheel/default actions require enabled scrollbar models. The CPU
    // theme has zero visual thickness; that does not disable content scrolling.
    view.setCanHaveScrollbars(true);
    view.setScrollbarModes(ScrollbarMode::Auto, ScrollbarMode::Auto, false, false);
    view.setTransparent(false);
    view.setBaseBackgroundColor(Color::white);
}

void ScarletFrameLoaderClient::dispatchDecidePolicyForNewWindowAction(const NavigationAction&, const ResourceRequest&, FormState*, const String&, std::optional<HitTestResult>&&, FramePolicyFunction&& completion)
{
    if (m_frame.isMainFrame())
        m_state.error = "Popup windows are unavailable in Scarlet WebView."_s;
    completion(PolicyAction::Ignore);
}

void ScarletFrameLoaderClient::dispatchDecidePolicyForNavigationAction(const NavigationAction&, const ResourceRequest& request, const ResourceResponse&, FormState*, const String&, std::optional<NavigationIdentifier>, std::optional<HitTestResult>&&, bool, NavigationUpgradeToHTTPSBehavior, SandboxFlags, PolicyDecisionMode, FramePolicyFunction&& completion)
{
    bool supported = request.url().protocolIsInHTTPFamily() || request.url().protocolIsData() || request.url().protocolIsBlob() || (request.url().isAboutBlank() || request.url().isAboutSrcDoc());
    if (!supported && m_frame.isMainFrame())
        m_state.error = "Unsupported navigation URL scheme."_s;
    completion(supported ? PolicyAction::Use : PolicyAction::Ignore);
}

void ScarletFrameLoaderClient::updateSandboxFlags(SandboxFlags)
{
}

void ScarletFrameLoaderClient::updateOpener(std::optional<FrameIdentifier>)
{
}

void ScarletFrameLoaderClient::setPrinting(bool, FloatSize, FloatSize, float, AdjustViewSize)
{
}

void ScarletFrameLoaderClient::dispatchWillSendSubmitEvent(Ref<FormState>&&)
{
}

void ScarletFrameLoaderClient::dispatchWillSubmitForm(FormState&, URL&&, String&&, CompletionHandler<void()>&& completionHandler)
{
    completionHandler();
}

Ref<DocumentLoader> ScarletFrameLoaderClient::createDocumentLoader(ResourceRequest&& request, SubstituteData&& substituteData, ResourceRequest&& originalRequest)
{
    return DocumentLoader::create(WTF::move(request), WTF::move(substituteData), WTF::move(originalRequest));
}

Ref<DocumentLoader> ScarletFrameLoaderClient::createDocumentLoader(ResourceRequest&& request, SubstituteData&& substituteData)
{
    return DocumentLoader::create(WTF::move(request), WTF::move(substituteData), { });
}

RefPtr<LocalFrame> ScarletFrameLoaderClient::createFrame(const AtomString& name, HTMLFrameOwnerElement& owner)
{
    RefPtr parent = owner.document().frame();
    RefPtr page = parent ? parent->page() : nullptr;
    if (!page)
        return nullptr;
    auto sandbox = owner.sandboxFlags();
    sandbox.add(parent->effectiveSandboxFlags());
    auto referrerPolicy = owner.referrerPolicy();
    if (referrerPolicy == ReferrerPolicy::EmptyString) {
        if (RefPtr document = page->localTopDocument())
            referrerPolicy = document->referrerPolicy();
    }
    auto child = LocalFrame::createSubframe(*page,
        [state = &m_state](LocalFrame& frame, FrameLoader& loader) {
            return makeUniqueRefWithoutRefCountedCheck<ScarletFrameLoaderClient>(frame, loader, *state);
        }, generateFrameIdentifier(), sandbox, referrerPolicy, owner, FrameTreeSyncData::create());
    child->tree().setSpecifiedName(name);
    child->init();
    // Initializing about:blank can execute script which removes the frame.
    if (!child->page())
        return nullptr;
    m_state.needsRenderingUpdate = true;
    return child;
}

RefPtr<Widget> ScarletFrameLoaderClient::createPlugin(HTMLPlugInElement&, const URL&, const Vector<AtomString>&, const Vector<AtomString>&, const String&, bool)
{
    return nullptr;
}

bool ScarletFrameLoaderClient::hasWebView() const
{
    return true; // mainly for assertions
}

void ScarletFrameLoaderClient::makeRepresentation(DocumentLoader*)
{
}

#if PLATFORM(IOS_FAMILY)

bool ScarletFrameLoaderClient::forceLayoutOnRestoreFromBackForwardCache()
{
    return false;
}

#endif

void ScarletFrameLoaderClient::forceLayoutForNonHTML()
{
}

void ScarletFrameLoaderClient::setCopiesOnScroll()
{
}

void ScarletFrameLoaderClient::detachedFromParent2()
{
}

void ScarletFrameLoaderClient::detachedFromParent3()
{
}

void ScarletFrameLoaderClient::convertMainResourceLoadToDownload(DocumentLoader*, const ResourceRequest&, const ResourceResponse&)
{
}

void ScarletFrameLoaderClient::assignIdentifierToInitialRequest(ResourceLoaderIdentifier, DocumentLoader*, const ResourceRequest&)
{
}

bool ScarletFrameLoaderClient::shouldUseCredentialStorage(DocumentLoader*, ResourceLoaderIdentifier)
{
    return false;
}

void ScarletFrameLoaderClient::dispatchWillSendRequest(DocumentLoader*, ResourceLoaderIdentifier, ResourceRequest&, const ResourceResponse&)
{
}

void ScarletFrameLoaderClient::dispatchDidReceiveAuthenticationChallenge(DocumentLoader*, ResourceLoaderIdentifier, const AuthenticationChallenge&)
{
}

#if USE(PROTECTION_SPACE_AUTH_CALLBACK)

bool ScarletFrameLoaderClient::canAuthenticateAgainstProtectionSpace(DocumentLoader*, ResourceLoaderIdentifier, const ProtectionSpace&)
{
    return false;
}

#endif

#if PLATFORM(IOS_FAMILY)

RetainPtr<CFDictionaryRef> ScarletFrameLoaderClient::connectionProperties(DocumentLoader*, ResourceLoaderIdentifier)
{
    return nullptr;
}

#endif

void ScarletFrameLoaderClient::dispatchDidReceiveResponse(DocumentLoader*, ResourceLoaderIdentifier, const ResourceResponse&)
{
}

void ScarletFrameLoaderClient::dispatchDidReceiveContentLength(DocumentLoader*, ResourceLoaderIdentifier, int)
{
}

void ScarletFrameLoaderClient::dispatchDidFinishLoading(DocumentLoader*, ResourceLoaderIdentifier)
{
}

#if ENABLE(DATA_DETECTION)

void ScarletFrameLoaderClient::dispatchDidFinishDataDetection(NSArray *)
{
}

#endif

void ScarletFrameLoaderClient::dispatchDidFailLoading(DocumentLoader*, ResourceLoaderIdentifier, const ResourceError&)
{
}

bool ScarletFrameLoaderClient::dispatchDidLoadResourceFromMemoryCache(DocumentLoader*, const ResourceRequest&, const ResourceResponse&, int)
{
    return false;
}

void ScarletFrameLoaderClient::dispatchDidDispatchOnloadEvents()
{
}

void ScarletFrameLoaderClient::dispatchDidReceiveServerRedirectForProvisionalLoad()
{
}

void ScarletFrameLoaderClient::dispatchDidCancelClientRedirect()
{
}

void ScarletFrameLoaderClient::dispatchWillPerformClientRedirect(const URL&, double, WallTime, LockBackForwardList)
{
}

void ScarletFrameLoaderClient::dispatchDidChangeLocationWithinPage()
{
}

void ScarletFrameLoaderClient::dispatchDidPushStateWithinPage()
{
}

void ScarletFrameLoaderClient::dispatchDidReplaceStateWithinPage()
{
}

void ScarletFrameLoaderClient::dispatchDidPopStateWithinPage()
{
}

void ScarletFrameLoaderClient::dispatchWillClose()
{
}

void ScarletFrameLoaderClient::dispatchDidStartProvisionalLoad()
{
    if (!m_frame.isMainFrame())
        return;
    m_state.loading = true;
    m_state.error = { };
}

void ScarletFrameLoaderClient::dispatchDidReceiveTitle(const StringWithDirection&)
{
}

void ScarletFrameLoaderClient::dispatchDidCommitLoad(const std::optional<BackForwardCacheCommitData>&)
{
    if (m_frame.isMainFrame()) {
        m_state.needsPaint = true;
        m_state.fullPaint = true;
    }
}

void ScarletFrameLoaderClient::dispatchDidFailProvisionalLoad(const ResourceError& error, WillContinueLoading, WillInternallyHandleFailure)
{
    if (!m_frame.isMainFrame())
        return;
    m_state.loading = false;
    m_state.error = error.localizedDescription();
}

void ScarletFrameLoaderClient::dispatchDidFailLoad(const ResourceError& error)
{
    if (!m_frame.isMainFrame())
        return;
    m_state.loading = false;
    m_state.error = error.localizedDescription();
}

void ScarletFrameLoaderClient::dispatchDidFinishDocumentLoad()
{
}

void ScarletFrameLoaderClient::dispatchDidFinishLoad()
{
    if (m_frame.isMainFrame())
        m_state.loading = false;
    m_state.needsRenderingUpdate = true;
}

void ScarletFrameLoaderClient::dispatchDidReachLayoutMilestone(OptionSet<LayoutMilestone>)
{
}

void ScarletFrameLoaderClient::dispatchDidReachVisuallyNonEmptyState()
{
}

LocalFrame* ScarletFrameLoaderClient::dispatchCreatePage(const NavigationAction&, NewFrameOpenerPolicy, const String&)
{
    return nullptr;
}

void ScarletFrameLoaderClient::dispatchShow()
{
}

void ScarletFrameLoaderClient::dispatchDecidePolicyForResponse(const ResourceResponse& response, const ResourceRequest&, const String& download, FramePolicyFunction&& completion)
{
    bool supported = download.isNull() && !response.isAttachment() && canShowMIMEType(response.mimeType());
    if (!supported && m_frame.isMainFrame())
        m_state.error = "Downloads and unsupported document MIME types are unavailable."_s;
    completion(supported ? PolicyAction::Use : PolicyAction::Ignore);
}

void ScarletFrameLoaderClient::cancelPolicyCheck()
{
}

void ScarletFrameLoaderClient::dispatchUnableToImplementPolicy(const ResourceError& error)
{
    if (!m_frame.isMainFrame())
        return;
    m_state.loading = false;
    m_state.error = error.localizedDescription();
}

void ScarletFrameLoaderClient::revertToProvisionalState(DocumentLoader*)
{
}

void ScarletFrameLoaderClient::setMainDocumentError(DocumentLoader*, const ResourceError& error)
{
    if (!m_frame.isMainFrame())
        return;
    m_state.error = error.localizedDescription();
}

void ScarletFrameLoaderClient::setMainFrameDocumentReady(bool)
{
}

void ScarletFrameLoaderClient::startDownload(const ResourceRequest&, const String&, FromDownloadAttribute)
{
    if (m_frame.isMainFrame())
        m_state.error = "Downloads are unavailable in Scarlet WebView."_s;
}

void ScarletFrameLoaderClient::willChangeTitle(DocumentLoader*)
{
}

void ScarletFrameLoaderClient::didChangeTitle(DocumentLoader*)
{
}

void ScarletFrameLoaderClient::willReplaceMultipartContent()
{
}

void ScarletFrameLoaderClient::didReplaceMultipartContent()
{
}

void ScarletFrameLoaderClient::committedLoad(DocumentLoader* loader, const SharedBuffer& data)
{
    if (loader)
        loader->commitData(data);
}

void ScarletFrameLoaderClient::finishedLoading(DocumentLoader*)
{
}

bool ScarletFrameLoaderClient::shouldFallBack(const ResourceError&) const
{
    return false;
}

void ScarletFrameLoaderClient::loadStorageAccessQuirksIfNeeded()
{
}

bool ScarletFrameLoaderClient::canHandleRequest(const ResourceRequest& request) const
{
    return request.url().protocolIsInHTTPFamily() || request.url().protocolIsData() || request.url().protocolIsBlob() || (request.url().isAboutBlank() || request.url().isAboutSrcDoc());
}

bool ScarletFrameLoaderClient::canShowMIMEType(const String& mime) const
{
    return MIMETypeRegistry::isSupportedNonImageMIMEType(mime) || MIMETypeRegistry::isSupportedImageMIMEType(mime);
}

bool ScarletFrameLoaderClient::canShowMIMETypeAsHTML(const String& mime) const
{
    return MIMETypeRegistry::isSupportedNonImageMIMEType(mime);
}

bool ScarletFrameLoaderClient::representationExistsForURLScheme(StringView) const
{
    return false;
}

String ScarletFrameLoaderClient::generatedMIMETypeForURLScheme(StringView) const
{
    return emptyString();
}

void ScarletFrameLoaderClient::frameLoadCompleted()
{
}

void ScarletFrameLoaderClient::restoreViewState()
{
}

void ScarletFrameLoaderClient::provisionalLoadStarted()
{
}

void ScarletFrameLoaderClient::didFinishLoad()
{
}

void ScarletFrameLoaderClient::prepareForDataSourceReplacement()
{
}

void ScarletFrameLoaderClient::updateCachedDocumentLoader(DocumentLoader&)
{
}

void ScarletFrameLoaderClient::setTitle(const StringWithDirection&, const URL&)
{
}

String ScarletFrameLoaderClient::userAgent(const URL&) const
{
    // Apply a host-selected compatibility identity to both navigator.userAgent
    // and WebCore's HTTP requests, including child frames. Keep the native
    // identity unless the embedding explicitly requests an override.
    if (auto* value = std::getenv("SCARLET_WEBVIEW_USER_AGENT")) {
        auto userAgent = String::fromUTF8(value);
        if (!userAgent.isEmpty() && userAgent.length() <= 1024 && !userAgent.contains('\r') && !userAgent.contains('\n'))
            return userAgent;
    }
    return "Mozilla/5.0 (Scarlet; AArch64) AppleWebKit/605.1.15 Myrica/0.1"_s;
}

void ScarletFrameLoaderClient::savePlatformDataToCachedFrame(CachedFrame*)
{
}

void ScarletFrameLoaderClient::transitionToCommittedFromCachedFrame(CachedFrame*)
{
}

#if PLATFORM(IOS_FAMILY)

void ScarletFrameLoaderClient::didRestoreFrameHierarchyForCachedFrame()
{
}

#endif

void ScarletFrameLoaderClient::transitionToCommittedForNewPage(InitializingIframe)
{
    auto size = m_frame.view() ? m_frame.view()->size() : IntSize { 800, 600 };
    // The old document can still own a built render tree here. createView()
    // first detaches its old view/document through the native frame lifecycle.
    m_frame.createView(size, Color::white, { }, false,
        ScrollbarMode::Auto, false, ScrollbarMode::Auto, false);
    configureScarletFrameView(*m_frame.view());
}

void ScarletFrameLoaderClient::didRestoreFromBackForwardCache()
{
}

void ScarletFrameLoaderClient::updateGlobalHistory()
{
}

void ScarletFrameLoaderClient::updateGlobalHistoryRedirectLinks()
{
}

ShouldGoToHistoryItem ScarletFrameLoaderClient::shouldGoToHistoryItem(HistoryItem&, IsSameDocumentNavigation) const
{
    return ShouldGoToHistoryItem::Yes;
}

bool ScarletFrameLoaderClient::supportsAsyncShouldGoToHistoryItem() const
{
    return false;
}

void ScarletFrameLoaderClient::shouldGoToHistoryItemAsync(HistoryItem&, CompletionHandler<void(ShouldGoToHistoryItem)>&& completion) const
{
    completion(ShouldGoToHistoryItem::Yes);
}

void ScarletFrameLoaderClient::dispatchGoToBackForwardItemAtIndex(int steps)
{
    if (RefPtr page = m_frame.page())
        page->backForward().goBackOrForward(steps);
}

void ScarletFrameLoaderClient::dispatchEnqueueHistoryTraversalDelta(int steps)
{
    m_frame.navigationScheduler().scheduleHistoryNavigation(steps);
}

void ScarletFrameLoaderClient::saveViewStateToItem(HistoryItem&)
{
}

bool ScarletFrameLoaderClient::canCachePage() const
{
    return false;
}

ObjectContentType ScarletFrameLoaderClient::objectContentType(const URL&, const String&)
{
    return ObjectContentType::None;
}

AtomString ScarletFrameLoaderClient::overrideMediaType() const
{
    return nullAtom();
}

void ScarletFrameLoaderClient::redirectDataToPlugin(Widget&)
{
}

void ScarletFrameLoaderClient::dispatchDidClearWindowObjectInWorld(DOMWrapperWorld&)
{
}

#if PLATFORM(COCOA)

RemoteAXObjectRef ScarletFrameLoaderClient::accessibilityRemoteObject()
{
    return nullptr;
}

IntPoint ScarletFrameLoaderClient::accessibilityRemoteFrameOffset()
{
    return { };
}

#if ENABLE(ACCESSIBILITY_ISOLATED_TREE)
void ScarletFrameLoaderClient::setIsolatedTree(Ref<WebCore::AXIsolatedTree>&&)
{
}

RefPtr<WebCore::AXIsolatedTree> ScarletFrameLoaderClient::isolatedTree() const
{
    return nullptr;
}
#endif

void ScarletFrameLoaderClient::willCacheResponse(DocumentLoader*, ResourceLoaderIdentifier, NSCachedURLResponse *response, CompletionHandler<void(NSCachedURLResponse *)>&& completionHandler) const
{
    completionHandler(response);
}

#endif

bool ScarletFrameLoaderClient::isEmptyFrameLoaderClient() const
{
    return false;
}

void ScarletFrameLoaderClient::prefetchDNS(const String&)
{
}

RefPtr<HistoryItem> ScarletFrameLoaderClient::createHistoryItemTree(bool clipAtTarget, BackForwardItemIdentifier itemID) const
{
    return m_frame.rootFrame().loader().history().createItemTree(m_frame, clipAtTarget, itemID);
}

#if USE(QUICK_LOOK)

RefPtr<LegacyPreviewLoaderClient> ScarletFrameLoaderClient::createPreviewLoaderClient(const String&, const String&)
{
    return nullptr;
}

#endif

bool ScarletFrameLoaderClient::hasFrameSpecificStorageAccess()
{
    return false;
}

void ScarletFrameLoaderClient::revokeFrameSpecificStorageAccess()
{
}

void ScarletFrameLoaderClient::dispatchLoadEventToOwnerElementInAnotherProcess()
{
}

inline ScarletFrameNetworkingContext::ScarletFrameNetworkingContext(LocalFrame& frame)
    : FrameNetworkingContext { &frame }
{
}

Ref<FrameNetworkingContext> ScarletFrameLoaderClient::createNetworkingContext()
{
    return ScarletFrameNetworkingContext::create(m_frame);
}

void ScarletFrameLoaderClient::sendH2Ping(const URL& url, CompletionHandler<void(std::expected<Seconds, ResourceError>&&)>&& completionHandler)
{
    completionHandler(makeUnexpected(internalError(url)));
}


} // namespace WebCore
