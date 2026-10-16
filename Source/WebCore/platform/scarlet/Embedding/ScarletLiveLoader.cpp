/* HTTP transport adapter for persistent, native Scarlet WebCore Pages.
 * WebCore constructs requests and enforces navigation/redirect/response policy;
 * the embedding host supplies one verified HTTP hop at a time.
 */
#include "config.h"
#include "ScarletLiveLoader.h"
#include "ScarletStorage.h"
#include "CookieJar.h"
#include "SameSiteInfo.h"

#include "CachedResource.h"
#include "BlobRegistry.h"
#include "BlobRegistryImpl.h"
#include "BlobPart.h"
#include "BlobDataFileReference.h"
#include "PolicyContainer.h"
#include <charconv>
#include <wtf/TZoneMallocInlines.h>
#include "CacheValidation.h"
#include "CrossOriginAccessControl.h"
#include "CrossOriginEmbedderPolicy.h"
#include "Document.h"
#include "DocumentPage.h"
#include "DocumentIsolationPolicy.h"
#include "FormData.h"
#include "FrameLoader.h"
#include "FrameTree.h"
#include "HTTPHeaderMap.h"
#include "HTTPParsers.h"
#include "LoaderStrategy.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "NetworkLoadMetrics.h"
#include "NetworkingContext.h"
#include "OriginAccessPatterns.h"
#include "Page.h"
#include "PlatformStrategies.h"
#include "ResourceError.h"
#include "ResourceLoaderTypes.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SharedBuffer.h"
#include "SecurityOrigin.h"
#include "SubresourceLoader.h"
#include <wtf/Assertions.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Function.h>
#include <wtf/MainThread.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#if !PLATFORM(SCARLET)
#error "ScarletLiveLoader belongs to the native Scarlet WebCore embedding."
#endif

namespace WebCore {
using namespace WTF::StringLiterals;

namespace {

constexpr size_t maxHeaderCount = 4096;
constexpr size_t maxStringBytes = 1024 * 1024;
constexpr size_t maxHeaderBytes = 1024 * 1024;
constexpr size_t maxBodyBytes = 128 * 1024 * 1024;
constexpr size_t maxActiveLoads = 4096;

enum class LiveError : int {
    Blocked = 1, Cancelled, CannotShowURL, PolicyChange, ContentBlocker,
    ContentFilter, CannotShowMIMEType, FileUnavailable, HTTPSRedirectLoop,
    HTTPSOnly, PluginUnavailable, Unsupported, Transport,
};

ResourceError liveError(LiveError code, const URL& url, const String& message,
    ResourceError::Type type = ResourceError::Type::General)
{
    return { "ScarletWebView"_s, static_cast<int>(code), url, message, type };
}

ResourceError unsupportedError(const ResourceRequest& request, const String& message)
{
    return liveError(LiveError::Unsupported, request.url(), message);
}

bool hasCode(const ResourceError& error, LiveError code)
{
    return error.domain() == "ScarletWebView"_s && error.errorCode() == static_cast<int>(code);
}

[[noreturn]] void unsupportedService(const char* operation)
{
    // PlatformStrategies has no error-returning factory contract. An attempt
    // to instantiate an unavailable service must fail explicitly.
    fprintf(stderr, "SCARLET_WEBVIEW_UNSUPPORTED service=%s\n", operation);
    std::abort();
}

std::string utf8(const String& value)
{
    auto bytes = value.utf8();
    return { reinterpret_cast<const char*>(bytes.data()), bytes.length() };
}

SWVBytes span(const std::string& value)
{
    return { reinterpret_cast<const uint8_t*>(value.data()), value.size() };
}

std::string schedulingOrigin(const URL& url)
{
    auto port = url.port();
    return utf8(url.protocol().toString()) + "://[" + utf8(url.host().toString()) + "]:"
        + (port ? std::to_string(*port) : "default");
}

bool copyString(SWVBytes bytes, std::string& output)
{
    if (bytes.length > maxStringBytes || (bytes.length && !bytes.data))
        return false;
    if (!bytes.length) {
        output.clear();
        return true;
    }
    output.assign(reinterpret_cast<const char*>(bytes.data), bytes.length);
    // Strings cross the ABI as UTF-8, unlike arbitrary binary request bodies.
    return output.find('\0') == std::string::npos && !String::fromUTF8(output.c_str()).isNull();
}

String nativeString(const std::string& value)
{
    return String::fromUTF8(value.c_str());
}

struct HeaderData {
    std::string name;
    std::string value;
};

struct ResponseData {
    uint32_t status { 0 };
    int32_t transportError { 0 };
    std::string url;
    std::string statusText;
    std::string errorMessage;
    std::vector<HeaderData> headers;
    Vector<uint8_t> body;
    bool streaming { false };
};

bool copyResponse(const SWVResponse& input, ResponseData& output)
{
    output.transportError = input.transport_error;
    if (input.transport_error)
        return copyString(input.error_message, output.errorMessage);
    if (input.status_code < 200 || input.status_code > 599 || input.header_count > maxHeaderCount
        || (input.header_count && !input.headers) || input.body.length > maxBodyBytes
        || (input.body.length && !input.body.data))
        return false;
    output.status = input.status_code;
    if (!copyString(input.final_url, output.url) || !copyString(input.status_text, output.statusText))
        return false;
    output.headers.resize(input.header_count);
    size_t headerBytes = 0;
    for (size_t i = 0; i < input.header_count; ++i) {
        if (input.headers[i].name.length > maxHeaderBytes - headerBytes)
            return false;
        headerBytes += input.headers[i].name.length;
        if (input.headers[i].value.length > maxHeaderBytes - headerBytes)
            return false;
        headerBytes += input.headers[i].value.length;
        if (!copyString(input.headers[i].name, output.headers[i].name)
            || !copyString(input.headers[i].value, output.headers[i].value))
            return false;
    }
    if (input.body.length)
        output.body.append(std::span<const uint8_t> { input.body.data, input.body.length });
    return true;
}

enum class Phase {
    Queued, AwaitingResponse, ResponsePending, PolicyPending, PayloadReady,
    DeliveringPayload, FinishPending, DeliveringFinish, RedirectPending, LocalPending,
};

struct Load {
    RefPtr<SubresourceLoader> loader;
    RefPtr<CachedResource> resource;
    Page* page { nullptr }; // Cleared only after the loader's native cancellation.
    uint64_t registration { 0 };
    uint64_t id { 0 };
    Phase phase { Phase::Queued };
    ResourceLoadPriority priority { ResourceLoadPriority::Low };
    std::optional<ResponseData> response;
    std::string exportedURL;
    bool transportDetached { false };
    bool streamFinished { true };
    size_t receivedBodyBytes { 0 };
};

struct PollStorage {
    std::string url;
    std::string method;
    std::vector<HeaderData> headerData;
    std::vector<SWVHeader> headers;
    std::vector<uint8_t> body;
};

struct LivePage {
    void* identity { nullptr };
    uint64_t registration { 0 };
    unsigned initializing { 0 };
    bool unregistering { false };
    LoadSchedulingMode schedulingMode { LoadSchedulingMode::Direct };
    std::deque<uint64_t> cancellations;
    PollStorage pollStorage;
    std::string error;
};

class ScarletLiveLoaderStrategy final : public LoaderStrategy {
public:
    void registerPage(Page& page, void* identity)
    {
        RELEASE_ASSERT(WTF::isMainThread() && identity && !m_pages.contains(&page));
        auto state = std::make_unique<LivePage>();
        state->identity = identity;
        state->schedulingMode = page.loadSchedulingMode();
        RELEASE_ASSERT(m_nextRegistration && m_nextRegistration != std::numeric_limits<uint64_t>::max());
        state->registration = m_nextRegistration++;
        m_pages.emplace(&page, std::move(state));
    }

    void unregisterPage(Page& page)
    {
        RELEASE_ASSERT(WTF::isMainThread());
        auto found = m_pages.find(&page);
        RELEASE_ASSERT(found != m_pages.end());
        found->second->unregistering = true;
        // cancel invokes client callbacks and remove(), so iterate retained
        // records rather than invalidating an ownership-map iterator.
        auto loads = loadsForPage(page);
        for (auto& load : loads) {
            if (!load->loader->reachedTerminalState())
                load->loader->cancel();
        }
        m_pages.erase(&page);
    }

    void loadResource(LocalFrame& frame, CachedResource& resource, ResourceRequest&& request,
        const ResourceLoaderOptions& options, CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& completion) final
    {
        RELEASE_ASSERT(WTF::isMainThread());
        auto* page = frame.page();
        auto state = m_pages.find(page);
        if (state == m_pages.end() || state->second->unregistering || m_loads.size() + m_initializing >= maxActiveLoads) {
            resource.setResourceError(unsupportedError(request, "Request has no registered live Page or exceeds the active load limit"_s));
            completion(nullptr);
            return;
        }
        if (!request.url().protocolIsInHTTPFamily() && !request.url().protocolIsData() && !request.url().protocolIsBlob()) {
            auto error = unsupportedError(request, "Only HTTP(S), blob and native data URLs are supported by this embedding"_s);
            state->second->error = utf8(error.localizedDescription());
            resource.setResourceError(error);
            completion(nullptr);
            return;
        }

        ++state->second->initializing;
        ++m_initializing;
        auto registration = state->second->registration;
        SubresourceLoader::create(frame, resource, WTF::move(request), options,
            [this, page, registration, protectedResource = Ref { resource }, completion = WTF::move(completion)](RefPtr<SubresourceLoader>&& loader) mutable {
                RELEASE_ASSERT(m_initializing);
                --m_initializing;
                auto state = m_pages.find(page);
                if (state != m_pages.end() && state->second->registration == registration) {
                    RELEASE_ASSERT(state->second->initializing);
                    --state->second->initializing;
                }
                if (!loader) {
                    completion(nullptr);
                    return;
                }
                auto load = std::make_shared<Load>();
                load->loader = loader;
                load->resource = protectedResource.ptr();
                load->page = page;
                load->registration = registration;
                load->priority = loader->request().priority();
                load->phase = (loader->request().url().protocolIsData() || loader->request().url().protocolIsBlob()) ? Phase::LocalPending : Phase::Queued;
                if (load->phase == Phase::Queued && state != m_pages.end()
                    && state->second->registration == registration && !state->second->unregistering)
                    assignIdentifier(load);
                m_loads.emplace(loader.get(), load);
                // CachedResource registers its real m_loader in this caller
                // completion. No transport delivery may precede it.
                completion(WTF::move(loader));
                state = m_pages.find(page);
                if (state == m_pages.end() || state->second->registration != registration || state->second->unregistering) {
                    if (isActive(load))
                        load->loader->cancel();
                }
            });
    }

    void loadResourceSynchronously(FrameLoader&, ResourceLoaderIdentifier, const ResourceRequest& request,
        ClientCredentialPolicy, const FetchOptions&, const HTTPHeaderMap&, ResourceError& error,
        ResourceResponse& response, Vector<uint8_t>& data) final
    {
        error = unsupportedError(request, "Synchronous network loads are unavailable; use asynchronous fetch or XHR"_s);
        response = { };
        data.clear();
    }

    bool startKeepAliveLoadForWebKitLegacy(FrameLoader&, const ResourceRequest& request,
        const ResourceLoaderOptions&, CompletionHandler<void(const ResourceError&, const ResourceResponse&)>&& completion) final
    {
        auto error = unsupportedError(request, "Detached keep-alive loads are unavailable"_s);
        ResourceResponse response;
        completion(error, response);
        return true;
    }

    void preconnectTo(FrameLoader&, ResourceRequest&& request, StoredCredentialsPolicy,
        ShouldPreconnectAsFirstParty, PreconnectCompletionHandler&& completion) final
    {
        completion(unsupportedError(request, "Transport preconnection is unavailable in ABI v1"_s));
    }

    void pageLoadCompleted(Page&) final { }
    void browsingContextRemoved(LocalFrame&) final { }

    void remove(ResourceLoader* loader) final
    {
        RELEASE_ASSERT(WTF::isMainThread());
        auto found = m_loads.find(loader);
        if (found == m_loads.end()) {
            // Initial native policy rejection can release before create's
            // callback, when no strategy ownership record exists yet.
            RELEASE_ASSERT(m_initializing && loader->reachedTerminalState());
            return;
        }
        auto load = found->second;
        auto page = m_pages.find(load->page);
        if (page != m_pages.end() && page->second->registration == load->registration) {
            if (load->phase == Phase::AwaitingResponse)
                page->second->cancellations.push_back(load->id);
            if (load->resource->errorOccurred())
                page->second->error = utf8(load->resource->resourceError().localizedDescription());
        }
        m_ids.erase(load->id);
        if (!loader->reachedTerminalState()) {
            // Redirecting a main resource to data: finishes its network load
            // before continuing native decoding. Retain native ownership until
            // the later terminal releaseResources() calls remove again.
            load->transportDetached = true;
            load->id = 0;
            load->response.reset();
            load->phase = Phase::PolicyPending;
            return;
        }
        m_loads.erase(found);
        // remove is native completion/cancellation cleanup. Calling cancel
        // again here would re-enter ResourceLoader's teardown.
    }

    void setDefersLoading(ResourceLoader&, bool defers) final
    {
        RELEASE_ASSERT(WTF::isMainThread());
        if (!defers)
            pump();
    }

    void crossOriginRedirectReceived(ResourceLoader*, const URL&) final
    {
        // WebCore's willSendRequest callback has already checked and updated
        // the new request. The bridge issues only that returned request.
    }

    void servePendingRequests(ResourceLoadPriority) final { pump(); }
    void suspendPendingRequests() final
    {
        RELEASE_ASSERT(WTF::isMainThread());
        ++m_suspended;
    }
    void resumePendingRequests() final
    {
        RELEASE_ASSERT(WTF::isMainThread() && m_suspended);
        if (!--m_suspended)
            pump();
    }
    void setResourceLoadSchedulingMode(Page& page, LoadSchedulingMode mode) final
    {
        if (auto state = m_pages.find(&page); state != m_pages.end())
            state->second->schedulingMode = mode;
    }
    void prioritizeResourceLoads(const Vector<Ref<SubresourceLoader>>& loaders) final
    {
        for (auto& loader : loaders) {
            if (auto found = m_loads.find(loader.ptr()); found != m_loads.end())
                found->second->priority = ResourceLoadPriority::VeryHigh;
        }
    }
    void setCaptureExtraNetworkLoadMetricsEnabled(bool) final { }

    // The host verifies TLS and moves bytes. Keeping these false is essential:
    // WebCore then performs native CORS preflight, redirect and response checks.
    bool shouldPerformSecurityChecks() const final { return false; }
    bool havePerformedSecurityChecks(const ResourceResponse&) const final { return false; }
    bool isOnLine() const final { return true; }
    void addOnlineStateChangeListener(Function<void(bool)>&& listener) final
    {
        m_onlineStateListeners.append(WTF::move(listener));
    }
    void isResourceLoadFinished(CachedResource& resource, CompletionHandler<void(bool)>&& completion) final
    {
        completion(!resource.loader() || !m_loads.contains(resource.loader()));
    }

    ResourceError cancelledError(const ResourceRequest& request) const final
    {
        return liveError(LiveError::Cancelled, request.url(), "Request was cancelled"_s, ResourceError::Type::Cancellation);
    }
    ResourceError blockedError(const ResourceRequest& request) const final
    {
        return liveError(LiveError::Blocked, request.url(), "Request was blocked by WebCore policy"_s, ResourceError::Type::AccessControl);
    }
    bool isBlockedError(const ResourceError& error) const final
    {
        return hasCode(error, LiveError::Blocked) || hasCode(error, LiveError::ContentBlocker) || hasCode(error, LiveError::ContentFilter);
    }
    ResourceError blockedByContentBlockerError(const ResourceRequest& request) const final
    {
        return liveError(LiveError::ContentBlocker, request.url(), "Request was blocked by a content rule"_s, ResourceError::Type::AccessControl);
    }
    ResourceError cannotShowURLError(const ResourceRequest& request) const final
    {
        return liveError(LiveError::CannotShowURL, request.url(), "WebCore cannot display this URL"_s);
    }
    ResourceError interruptedForPolicyChangeError(const ResourceRequest& request) const final
    {
        return liveError(LiveError::PolicyChange, request.url(), "Load was interrupted by a policy change"_s, ResourceError::Type::Cancellation);
    }
#if ENABLE(CONTENT_FILTERING)
    ResourceError blockedByContentFilterError(const ResourceRequest& request) const final
    {
        return liveError(LiveError::ContentFilter, request.url(), "Request was blocked by a content filter"_s, ResourceError::Type::AccessControl);
    }
#endif
    ResourceError cannotShowMIMETypeError(const ResourceResponse& response) const final
    {
        return liveError(LiveError::CannotShowMIMEType, response.url(), "WebCore cannot display this response MIME type"_s);
    }
    ResourceError fileDoesNotExistError(const ResourceResponse& response) const final
    {
        return liveError(LiveError::FileUnavailable, response.url(), "File is unavailable in the live HTTP embedding"_s);
    }
    ResourceError httpsUpgradeRedirectLoopError(const ResourceRequest& request) const final
    {
        return liveError(LiveError::HTTPSRedirectLoop, request.url(), "HTTPS upgrade redirect loop"_s);
    }
    ResourceError httpNavigationWithHTTPSOnlyError(const ResourceRequest& request) const final
    {
        return liveError(LiveError::HTTPSOnly, request.url(), "HTTP navigation was blocked by HTTPS-only policy"_s, ResourceError::Type::AccessControl);
    }
    bool isHttpNavigationWithHTTPSOnlyError(const ResourceError& error) const final
    {
        return hasCode(error, LiveError::HTTPSOnly);
    }
    ResourceError pluginWillHandleLoadError(const ResourceResponse& response) const final
    {
        return liveError(LiveError::PluginUnavailable, response.url(), "Plugins are unavailable in the native embedding"_s);
    }

    int32_t poll(Page& page, SWVRequest& output)
    {
        RELEASE_ASSERT(WTF::isMainThread());
        output = { };
        auto state = m_pages.find(&page);
        if (state == m_pages.end())
            return SWV_INVALID_ARGUMENT;
        auto& storage = state->second->pollStorage;
        storage = { }; // The previous poll's spans expire only here.
        pump();
        state = m_pages.find(&page);
        if (state == m_pages.end())
            return SWV_INVALID_ARGUMENT;
        if (!state->second->cancellations.empty()) {
            output.request_id = state->second->cancellations.front();
            state->second->cancellations.pop_front();
            output.is_cancel = 1;
            return SWV_OK;
        }
        if (m_suspended)
            return SWV_EMPTY;

        while (auto load = nextQueued(page)) {
            if (!serializeRequest(*load, storage)) {
                fail(load, unsupportedError(load->loader->request(), "Request body requires files, blobs or streams, or exceeds ABI limits"_s));
                continue;
            }
            load->exportedURL = storage.url;
            load->phase = Phase::AwaitingResponse;
            output.request_id = load->id;
            output.url = span(storage.url);
            output.method = span(storage.method);
            output.headers = storage.headers.data();
            output.header_count = storage.headers.size();
            output.body = { storage.body.data(), storage.body.size() };
            switch (load->loader->options().credentials) {
            case FetchOptions::Credentials::Omit: output.credentials_mode = 0; break;
            case FetchOptions::Credentials::SameOrigin: output.credentials_mode = 1; break;
            case FetchOptions::Credentials::Include: output.credentials_mode = 2; break;
            }
            return SWV_OK;
        }
        return SWV_EMPTY;
    }

    int32_t complete(Page& page, const SWVResponse& input, bool streaming = false)
    {
        RELEASE_ASSERT(WTF::isMainThread());
        auto found = m_ids.find(input.request_id);
        if (found == m_ids.end() || found->second->page != &page
            || found->second->loader->reachedTerminalState())
            return SWV_STALE_REQUEST;
        auto load = found->second;
        if (input.transport_error) {
            std::string message;
            if (!copyString(input.error_message, message))
                return SWV_INVALID_ARGUMENT;
            fail(load, liveError(LiveError::Transport, load->loader->request().url(), nativeString(message)));
            return SWV_OK;
        }
        if (load->phase != Phase::AwaitingResponse)
            return SWV_STALE_REQUEST;
        ResponseData response;
        if (!copyResponse(input, response))
            return SWV_INVALID_ARGUMENT;
        if (!response.transportError && response.url != load->exportedURL) {
            // A changed final URL proves the host followed a redirect outside
            // WebCore's security pipeline. Fail that load rather than accepting
            // bytes from an unchecked destination.
            fail(load, liveError(LiveError::Transport, load->loader->request().url(), "Host response URL differs from the exact requested HTTP hop"_s));
            return SWV_FAILED;
        }
        response.streaming = streaming;
        load->streamFinished = !streaming;
        load->receivedBodyBytes = response.body.size();
        load->response = std::move(response);
        load->phase = Phase::ResponsePending;
        pump();
        return SWV_OK;
    }

    int32_t append(Page& page, uint64_t id, SWVBytes bytes, bool finished)
    {
        RELEASE_ASSERT(WTF::isMainThread());
        auto found = m_ids.find(id);
        if (found == m_ids.end() || found->second->page != &page)
            return SWV_STALE_REQUEST;
        auto load = found->second;
        if (!isActive(load) || !load->response || !load->response->streaming || load->streamFinished)
            return SWV_STALE_REQUEST;
        if ((bytes.length && !bytes.data) || bytes.length > maxBodyBytes - load->receivedBodyBytes)
            return SWV_INVALID_ARGUMENT;
        load->receivedBodyBytes += bytes.length;
        if (bytes.length)
            load->response->body.append(std::span<const uint8_t> { bytes.data, bytes.length });
        load->streamFinished = finished;
        pump();
        return SWV_OK;
    }

    uint32_t pending(Page& page) const
    {
        auto state = m_pages.find(&page);
        if (state == m_pages.end())
            return 0;
        auto count = state->second->initializing;
        for (const auto& [loader, load] : m_loads) {
            if (load->page == &page)
                ++count;
        }
        return count;
    }

    std::string lastError(Page& page) const
    {
        auto state = m_pages.find(&page);
        return state == m_pages.end() ? std::string { } : state->second->error;
    }

private:
    void assignIdentifier(const std::shared_ptr<Load>& load)
    {
        RELEASE_ASSERT(m_nextID && m_nextID != std::numeric_limits<uint64_t>::max());
        load->id = m_nextID++;
        m_ids.emplace(load->id, load);
    }

    bool isActive(const std::shared_ptr<Load>& load) const
    {
        auto found = m_loads.find(load->loader.get());
        return found != m_loads.end() && found->second == load && !load->loader->reachedTerminalState();
    }

    std::vector<std::shared_ptr<Load>> loadsForPage(Page& page) const
    {
        std::vector<std::shared_ptr<Load>> result;
        for (const auto& [loader, load] : m_loads) {
            if (load->page == &page)
                result.push_back(load);
        }
        return result;
    }

    std::shared_ptr<Load> nextQueued(Page& page) const
    {
        std::shared_ptr<Load> candidate;
        auto pageState = m_pages.find(&page);
        bool prioritized = pageState != m_pages.end() && pageState->second->schedulingMode == LoadSchedulingMode::Prioritized;
        std::map<std::string, unsigned> inProgress;
        if (prioritized) {
            for (const auto& [loader, load] : m_loads) {
                if (load->page == &page && load->phase != Phase::Queued && load->phase != Phase::LocalPending
                    && !loader->reachedTerminalState())
                    ++inProgress[schedulingOrigin(loader->request().url())];
            }
        }
        for (const auto& [loader, load] : m_loads) {
            if (load->page != &page || load->phase != Phase::Queued || loader->defersLoading() || loader->reachedTerminalState())
                continue;
            if (prioritized) {
                auto count = inProgress.find(schedulingOrigin(loader->request().url()));
                auto inProgressForHost = count == inProgress.end() ? 0 : count->second;
                if (inProgressForHost >= 4 || (load->priority == ResourceLoadPriority::VeryLow && inProgressForHost))
                    continue;
            }
            if (!candidate || load->priority > candidate->priority || (load->priority == candidate->priority && load->id < candidate->id))
                candidate = load;
        }
        return candidate;
    }

    bool serializeRequest(const Load& load, PollStorage& storage)
    {
        storage = { };
        const auto& request = load.loader->request();
        if (request.url().string().length() > maxStringBytes || request.httpMethod().length() > maxStringBytes)
            return false;
        auto transportURL = request.url();
        transportURL.removeFragmentIdentifier();
        storage.url = utf8(transportURL.string());
        storage.method = utf8(request.httpMethod());
        if (storage.url.size() > maxStringBytes || storage.method.empty() || storage.method.size() > maxStringBytes
            || request.httpHeaderFields().size() > static_cast<int>(maxHeaderCount))
            return false;
        size_t headerBytes = 0;
        for (auto& header : request.httpHeaderFields()) {
            if (header.key.length() > maxHeaderBytes - headerBytes || header.value.length() > maxHeaderBytes - headerBytes)
                return false;
            storage.headerData.push_back({ utf8(header.key), utf8(header.value) });
            if (storage.headerData.back().name.size() > maxHeaderBytes - headerBytes)
                return false;
            headerBytes += storage.headerData.back().name.size();
            if (storage.headerData.back().value.size() > maxHeaderBytes - headerBytes)
                return false;
            headerBytes += storage.headerData.back().value.size();
        }
        if (request.allowCookies() && load.loader->options().storedCredentialsPolicy == StoredCredentialsPolicy::Use) {
            auto cookie = load.page->cookieJar().cookieRequestHeaderFieldValue(request.firstPartyForCookies(), SameSiteInfo::create(request), request.url(), CookieJar::shouldIncludeSecureCookies(request.url())).first;
            if (!cookie.isEmpty())
                storage.headerData.push_back({ "Cookie", utf8(cookie) });
        }
        if (auto* body = request.httpBody()) {
            for (auto& element : body->elements()) {
                auto* bytes = std::get_if<Vector<uint8_t>>(&element.data);
                if (!bytes || bytes->size() > maxBodyBytes - storage.body.size())
                    return false;
                storage.body.insert(storage.body.end(), bytes->begin(), bytes->end());
            }
        }
        storage.headers.reserve(storage.headerData.size());
        for (const auto& header : storage.headerData)
            storage.headers.push_back({ span(header.name), span(header.value) });
        return true;
    }

    void fail(const std::shared_ptr<Load>& load, const ResourceError& error)
    {
        if (auto state = m_pages.find(load->page); state != m_pages.end())
            state->second->error = utf8(error.localizedDescription());
        if (isActive(load))
            load->loader->didFail(error);
    }

    ResourceResponse makeResponse(const ResponseData& data)
    {
        ResourceResponse response { URL { URL { }, nativeString(data.url) }, String { },
            data.streaming ? -1 : static_cast<long long>(data.body.size()), String { } };
        response.setSource(ResourceResponse::Source::Network);
        // ABI v1 does not carry a certificate chain; the host must verify TLS.
        // Native main-resource clients still require the certificate-info
        // optional to exist, with its actual empty platform representation.
        response.includeCertificateInfo();
        response.setHTTPStatusCode(static_cast<int>(data.status));
        response.setHTTPStatusText(nativeString(data.statusText));
        for (const auto& header : data.headers)
            response.addHTTPHeaderField(nativeString(header.name), nativeString(header.value));
        auto contentType = response.httpHeaderField(HTTPHeaderName::ContentType);
        if (!contentType.isEmpty()) {
            response.setMimeType(extractMIMETypeFromMediaType(contentType).convertToASCIILowercase());
            response.setTextEncodingName(extractCharsetFromMediaType(contentType).toString());
        }
        if (data.streaming) {
            auto length = utf8(response.httpHeaderField(HTTPHeaderName::ContentLength).stripWhiteSpace());
            int64_t expected = -1;
            auto parsed = std::from_chars(length.data(), length.data() + length.size(), expected);
            if (parsed.ec == std::errc { } && parsed.ptr == length.data() + length.size() && expected >= 0)
                response.setExpectedContentLength(expected);
        }
        return response;
    }

    std::optional<ResourceError> checkResourcePolicy(const Load& load, const ResourceResponse& response)
    {
        const auto& options = load.loader->options();
        const auto& request = load.loader->request();
        RefPtr frame = load.loader->frame();
        RefPtr document = frame ? frame->document() : nullptr;
        if (options.mode == FetchOptions::Mode::Navigate && frame && !frame->isMainFrame()) {
            RefPtr localParent = dynamicDowncast<LocalFrame>(frame->tree().parent());
            if (RefPtr parentDocument = localParent ? localParent->document() : nullptr)
                return validateCrossOriginResourcePolicy(parentDocument->crossOriginEmbedderPolicy().value,
                    DocumentIsolationPolicy::None, parentDocument->securityOrigin(), request.url(), response,
                    ForNavigation::Yes, OriginAccessPatternsForWebProcess::singleton());
        }
        if (options.mode != FetchOptions::Mode::NoCors)
            return std::nullopt;
        // WebCore's legacy preflight loader uses default NoCors options, but
        // this is the internal CORS-preflight request, whose native checker
        // validates Access-Control-* after completion. It is not a no-CORS
        // resource fetch and must not be subjected to CORP.
        if (request.httpMethod() == "OPTIONS"_s && request.hasHTTPHeaderField(HTTPHeaderName::AccessControlRequestMethod))
            return std::nullopt;
        RefPtr origin = load.loader->origin();
        if (!origin && document)
            origin = &document->securityOrigin();
        if (!origin)
            return liveError(LiveError::Blocked, request.url(), "Resource response has no native requesting origin"_s, ResourceError::Type::AccessControl);
        auto coep = options.crossOriginEmbedderPolicy ? options.crossOriginEmbedderPolicy->value
            : document ? document->crossOriginEmbedderPolicy().value : CrossOriginEmbedderPolicyValue::UnsafeNone;
        auto documentPolicy = document ? document->documentIsolationPolicy() : DocumentIsolationPolicy::None;
        return validateCrossOriginResourcePolicy(coep, documentPolicy, *origin, request.url(), response,
            ForNavigation::No, OriginAccessPatternsForWebProcess::singleton());
    }

    void deliverPayload(const std::shared_ptr<Load>& load)
    {
        if (!isActive(load) || load->phase != Phase::PayloadReady || m_suspended || load->loader->defersLoading())
            return;
        RELEASE_ASSERT(load->response);
        load->phase = Phase::DeliveringPayload;
        // Keep the cloned payload alive across callbacks that may remove the
        // strategy ownership record or start another navigation.
        auto body = WTF::move(load->response->body);
        if (!body.isEmpty()) {
            // The ABI bytes were already cloned when completion was accepted.
            // Transfer that allocation into WebCore instead of copying the
            // complete image/script/media response again on its owner thread.
            auto length = body.size();
            auto buffer = SharedBuffer::create(WTF::move(body));
            load->loader->didReceiveData(buffer.get(), static_cast<long long>(length), DataPayloadBytes);
        }
        if (!isActive(load))
            return;
        if (!load->streamFinished) {
            load->phase = Phase::PayloadReady;
            return;
        }
        load->response.reset();
        load->phase = Phase::FinishPending;
        deliverFinish(load);
    }

    void deliverFinish(const std::shared_ptr<Load>& load)
    {
        if (!isActive(load) || load->phase != Phase::FinishPending || m_suspended || load->loader->defersLoading())
            return;
        load->phase = Phase::DeliveringFinish;
        // ABI v1 does not invent DNS/TLS timing or compressed wire length.
        NetworkLoadMetrics metrics;
        load->loader->didFinishLoading(metrics);
    }

    void deliverResponse(const std::shared_ptr<Load>& load)
    {
        RELEASE_ASSERT(load->response);
        if (load->response->transportError) {
            auto message = load->response->errorMessage.empty() ? "Host HTTP transport failed"_s : nativeString(load->response->errorMessage);
            fail(load, liveError(LiveError::Transport, load->loader->request().url(), message));
            return;
        }
        auto response = makeResponse(*load->response);
        if (response.containsInvalidHTTPHeaders()) {
            fail(load, liveError(LiveError::Transport, load->loader->request().url(), "Host returned invalid HTTP response headers"_s));
            return;
        }
        // Preserve distinct Set-Cookie fields, including redirect responses.
        // WebCore determines credential policy before the transport is polled.
        if (load->loader->options().storedCredentialsPolicy == StoredCredentialsPolicy::Use) {
            for (auto& header : load->response->headers) {
                if (equalLettersIgnoringASCIICase(nativeString(header.name), "set-cookie"_s))
                    storeScarletResponseCookie(load->loader->request(), nativeString(header.value));
            }
        }
        // New HTTP responses do not pass through CachedResourceLoader's
        // MemoryCache::Use branch, where WebCore applies CORP to cached loads.
        // Apply that same native validator before exposing new transport bytes.
        auto policyResponse = response;
        if (response.httpStatusCode() == 304) {
            if (auto* original = load->resource->resourceToRevalidate()) {
                policyResponse = original->response();
                updateResponseHeadersAfterRevalidation(policyResponse, response);
            }
        }
        if (auto error = checkResourcePolicy(*load, policyResponse)) {
            fail(load, *error);
            return;
        }
        if (response.isRedirection() && !response.httpHeaderField(HTTPHeaderName::Location).isEmpty()) {
            auto* frameLoader = load->loader->frameLoader();
            auto* networkingContext = frameLoader ? frameLoader->networkingContext() : nullptr;
            bool clearReferrer = !networkingContext || networkingContext->shouldClearReferrerOnHTTPSToHTTPRedirect();
            auto request = load->loader->request().redirectedRequest(response, clearReferrer);
            load->phase = Phase::RedirectPending;
            m_ids.erase(load->id);
            load->response.reset();
            load->loader->willSendRequest(WTF::move(request), response,
                [this, load](ResourceRequest&& request) mutable {
                    if (!isActive(load) || load->transportDetached)
                        return;
                    if (request.isNull()) {
                        load->loader->cancel();
                        return;
                    }
                    if (!request.url().protocolIsInHTTPFamily()) {
                        // ResourceLoader handles a main-navigation data URL
                        // itself inside willSendRequest; it removes this live
                        // network record before decoding that URL natively.
                        fail(load, unsupportedError(request, "Redirect destination is outside HTTP(S)"_s));
                        return;
                    }
                    load->loader->setRequest(WTF::move(request));
                    load->exportedURL.clear();
                    load->phase = Phase::Queued;
                    load->priority = load->loader->request().priority();
                    assignIdentifier(load);
                });
            return;
        }
        load->phase = Phase::PolicyPending;
        load->loader->didReceiveResponse(WTF::move(response), [this, load] {
            if (!isActive(load))
                return;
            load->phase = Phase::PayloadReady;
            deliverPayload(load);
        });
    }

    void pump()
    {
        if (m_suspended || m_pumping)
            return;
        m_pumping = true;
        std::vector<std::shared_ptr<Load>> loads;
        loads.reserve(m_loads.size());
        for (const auto& [loader, load] : m_loads)
            loads.push_back(load);
        for (auto& load : loads) {
            if (m_suspended || !isActive(load) || load->loader->defersLoading())
                continue;
            if (load->phase == Phase::ResponsePending)
                deliverResponse(load);
            else if (load->phase == Phase::PayloadReady)
                deliverPayload(load);
            else if (load->phase == Phase::FinishPending)
                deliverFinish(load);
            else if (load->phase == Phase::LocalPending) {
                // No caller is invoked until after CachedResource installed
                // its m_loader; only data URLs use native ResourceHandle-free
                // ResourceLoader::start here.
                load->phase = Phase::PolicyPending;
                load->loader->start();
            }
        }
        m_pumping = false;
    }

    std::map<Page*, std::unique_ptr<LivePage>> m_pages;
    std::map<ResourceLoader*, std::shared_ptr<Load>> m_loads;
    std::map<uint64_t, std::shared_ptr<Load>> m_ids;
    uint64_t m_nextID { 1 };
    uint64_t m_nextRegistration { 1 };
    unsigned m_initializing { 0 };
    unsigned m_suspended { 0 };
    bool m_pumping { false };
    Vector<Function<void(bool)>> m_onlineStateListeners;
};

class ScarletBlobRegistry final : public WebCore::BlobRegistry {
    WTF_MAKE_TZONE_ALLOCATED(ScarletBlobRegistry);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ScarletBlobRegistry);
private:
    void registerInternalFileBlobURL(const URL& url, Ref<WebCore::BlobDataFileReference>&& reference, const String&, const String& contentType) final { m_blobRegistry.registerInternalFileBlobURL(url, WTF::move(reference), contentType); }
    void registerInternalBlobURL(const URL& url, Vector<WebCore::BlobPart>&& parts, const String& contentType) final { m_blobRegistry.registerInternalBlobURL(url, WTF::move(parts), contentType); }
    void registerBlobURL(const URL& url, const URL& srcURL, const WebCore::PolicyContainer& policyContainer, const std::optional<WebCore::SecurityOriginData>& topOrigin) final { m_blobRegistry.registerBlobURL(url, srcURL, policyContainer, topOrigin); }
    void registerInternalBlobURLOptionallyFileBacked(const URL& url, const URL& srcURL, RefPtr<WebCore::BlobDataFileReference>&& reference, const String& contentType) final { m_blobRegistry.registerInternalBlobURLOptionallyFileBacked(url, srcURL, WTF::move(reference), contentType, { }); }
    void registerInternalBlobURLForSlice(const URL& url, const URL& srcURL, long long start, long long end, const String& contentType) final { m_blobRegistry.registerInternalBlobURLForSlice(url, srcURL, start, end, contentType); }
    void unregisterBlobURL(const URL& url, const std::optional<WebCore::SecurityOriginData>& topOrigin) final { m_blobRegistry.unregisterBlobURL(url, topOrigin); }
    String blobType(const URL& url) final { return m_blobRegistry.blobType(url); }
    unsigned long long blobSize(const URL& url) final { return m_blobRegistry.blobSize(url); }
    void writeBlobsToTemporaryFilesForIndexedDB(const Vector<String>& blobURLs, CompletionHandler<void(Vector<String>&& filePaths)>&& completionHandler) final { m_blobRegistry.writeBlobsToTemporaryFilesForIndexedDB(blobURLs, WTF::move(completionHandler)); }
    void registerBlobURLHandle(const URL& url, const std::optional<WebCore::SecurityOriginData>& topOrigin) final { m_blobRegistry.registerBlobURLHandle(url, topOrigin); }
    void unregisterBlobURLHandle(const URL& url, const std::optional<WebCore::SecurityOriginData>& topOrigin) final { m_blobRegistry.unregisterBlobURLHandle(url, topOrigin); }

    WebCore::BlobRegistryImpl* blobRegistryImpl() final { return &m_blobRegistry; }

    WebCore::BlobRegistryImpl m_blobRegistry;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(ScarletBlobRegistry);


class ScarletLivePlatformStrategies final : public PlatformStrategies {
public:
    ScarletLiveLoaderStrategy& loader() { return *m_loader; }
private:
    LoaderStrategy* createLoaderStrategy() final { return m_loader; }
    PasteboardStrategy* createPasteboardStrategy() final { unsupportedService("clipboard"); }
    MediaStrategy* createMediaStrategy() final { unsupportedService("media"); }
    BlobRegistry* createBlobRegistry() final { return new ScarletBlobRegistry; }
#if ENABLE(DECLARATIVE_WEB_PUSH)
    PushStrategy* createPushStrategy() final { unsupportedService("web push"); }
#endif
    // PlatformStrategies retains CheckedPtrs rather than ownership. These
    // process services intentionally outlive every Page and async callback.
    ScarletLiveLoaderStrategy* const m_loader { new ScarletLiveLoaderStrategy };
};

ScarletLivePlatformStrategies* liveStrategies { nullptr };

ScarletLiveLoaderStrategy& liveLoader()
{
    RELEASE_ASSERT(WTF::isMainThread() && liveStrategies);
    return liveStrategies->loader();
}

} // namespace

void initializeScarletLiveStrategies()
{
    RELEASE_ASSERT(WTF::isMainThread() && !liveStrategies && !hasPlatformStrategies());
    liveStrategies = new ScarletLivePlatformStrategies;
    setPlatformStrategies(liveStrategies);
    RELEASE_ASSERT(platformStrategies() == liveStrategies);
}

void registerScarletLivePage(Page& page, void* identity) { liveLoader().registerPage(page, identity); }
void unregisterScarletLivePage(Page& page) { liveLoader().unregisterPage(page); }
int32_t pollScarletLiveRequest(Page& page, SWVRequest& request) { return liveLoader().poll(page, request); }
int32_t completeScarletLiveRequest(Page& page, const SWVResponse& response) { return liveLoader().complete(page, response); }
int32_t startScarletLiveResponse(Page& page, const SWVResponse& response) { return liveLoader().complete(page, response, true); }
int32_t appendScarletLiveResponse(Page& page, uint64_t id, SWVBytes bytes, bool finished) { return liveLoader().append(page, id, bytes, finished); }
uint32_t pendingScarletLiveRequests(Page& page) { return liveLoader().pending(page); }
std::string scarletLiveLastError(Page& page) { return liveLoader().lastError(page); }

} // namespace WebCore
