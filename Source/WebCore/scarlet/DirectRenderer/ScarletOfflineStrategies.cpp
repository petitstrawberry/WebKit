// Real direct-WebCore strategy contracts for a permanently offline runner.
#include "config.h"
#include "ScarletOfflineStrategies.h"

#include "CachedResource.h"
#include "LoaderStrategy.h"
#include "PlatformStrategies.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SharedBuffer.h"
#include "SubresourceLoader.h"
#include <wtf/Assertions.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Function.h>
#include <wtf/MainThread.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <set>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <unistd.h>

#if !PLATFORM(SCARLET)
#error "ScarletOfflineStrategies belongs to the native Scarlet runner."
#endif

namespace WebCore {
using namespace WTF::StringLiterals;

namespace {

enum class OfflineError : int {
    Blocked = 1,
    Cancelled = 2,
    CannotShowURL = 3,
    PolicyChange = 4,
    ContentBlocker = 5,
    ContentFilter = 6,
    CannotShowMIMEType = 7,
    FileUnavailable = 8,
    HTTPSRedirectLoop = 9,
    HTTPSOnly = 10,
    PluginUnavailable = 11,
};

ResourceError offlineError(OfflineError code, const URL& url,
    ResourceError::Type type = ResourceError::Type::AccessControl)
{
    return { "ScarletOffline"_s, static_cast<int>(code), url,
        "URL loading is disabled in the native offline WebCore runner"_s, type };
}

bool hasOfflineCode(const ResourceError& error, OfflineError code)
{
    return error.domain() == "ScarletOffline"_s && error.errorCode() == static_cast<int>(code);
}

[[noreturn]] void unsupported(const char* operation)
{
    // exit runs the runner's registered output-transaction cleanup.
    fprintf(stderr, "SCARLET_WEBCORE_ERROR unsupported offline strategy operation: %s\n", operation);
    std::exit(1);
}

void snapshotRequire(bool condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "SCARLET_WEBCORE_ERROR snapshot: %s\n", message);
        std::exit(1);
    }
}

constexpr size_t maxSnapshotIndexBytes = 1024 * 1024;
constexpr size_t maxSnapshotResourceBytes = 32 * 1024 * 1024;
constexpr uint64_t maxSnapshotTotalBytes = 128 * 1024 * 1024;
constexpr size_t maxSnapshotResources = 512;
constexpr size_t maxSnapshotRequests = 2048;

std::string utf8String(const String& value)
{
    auto bytes = value.utf8();
    return { reinterpret_cast<const char*>(bytes.data()), bytes.length() };
}

std::string jsonQuoted(std::string_view value)
{
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            result += '\\';
            result += static_cast<char>(ch);
        } else if (ch < 32) {
            result += "\\u00";
            result += hex[ch >> 4];
            result += hex[ch & 15];
        } else
            result += static_cast<char>(ch);
    }
    return result + '"';
}

bool isHex(std::string_view value, size_t length)
{
    if (value.size() != length)
        return false;
    for (char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return true;
}

bool safeObjectPath(std::string_view path)
{
    if (!path.starts_with("objects/") || path.size() < 72)
        return false;
    auto name = path.substr(8);
    if (!isHex(name.substr(0, 64), 64))
        return false;
    if (name.size() == 64)
        return true;
    if (name[64] != '.' || name.size() > 80)
        return false;
    for (char ch : name.substr(65)) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')))
            return false;
    }
    return name.size() > 65;
}

uint64_t decimal(std::string_view value)
{
    uint64_t number = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    snapshotRequire(!value.empty() && result.ec == std::errc { } && result.ptr == value.data() + value.size(), "invalid integer in native index");
    return number;
}

std::vector<std::string> fields(std::string_view line)
{
    std::vector<std::string> result;
    size_t offset = 0;
    while (true) {
        auto end = line.find('\t', offset);
        result.emplace_back(line.substr(offset, end == std::string_view::npos ? end : end - offset));
        if (end == std::string_view::npos)
            return result;
        offset = end + 1;
    }
}

std::vector<uint8_t> readSnapshotFD(int fd, size_t limit)
{
    struct stat info { };
    snapshotRequire(fd >= 0 && !fstat(fd, &info) && S_ISREG(info.st_mode), "snapshot object is not a readable regular file");
    snapshotRequire(info.st_size >= 0 && static_cast<uint64_t>(info.st_size) <= limit, "snapshot object exceeds byte limit");
    std::vector<uint8_t> data(static_cast<size_t>(info.st_size));
    size_t offset = 0;
    while (offset < data.size()) {
        auto count = read(fd, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        snapshotRequire(count > 0, "snapshot object read failed or truncated");
        offset += static_cast<size_t>(count);
    }
    uint8_t excess = 0;
    snapshotRequire(read(fd, &excess, 1) == 0, "snapshot object grew while reading");
    snapshotRequire(!close(fd), "snapshot object close failed");
    return data;
}

std::string hexBytes(std::span<const uint8_t> bytes)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

std::string fnv1a64(std::span<const uint8_t> bytes)
{
    uint64_t hash = 14695981039346656037ULL;
    for (auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    uint8_t encoded[8];
    for (unsigned i = 0; i < 8; ++i)
        encoded[i] = static_cast<uint8_t>(hash >> ((7 - i) * 8));
    return hexBytes(encoded);
}

std::string nativeSHA256(std::span<const uint8_t> bytes)
{
    uint8_t digest[SHA256_DIGEST_LENGTH];
    snapshotRequire(SHA256(bytes.data(), bytes.size(), digest), "native SHA256 failed");
    return hexBytes(digest);
}

struct SnapshotEntry {
    std::string url;
    std::string path;
    std::string mime;
    std::string charset;
    size_t bytes;
    std::string sha256;
    std::string fnv;
    std::string allowOrigin;
    std::string resourcePolicy;
};

class SnapshotStore {
public:
    SnapshotStore(const char* root, const char* index, const char* documentURL)
    {
        snapshotRequire(documentURL, "snapshot document URL is missing");
        m_documentURL = documentURL;
        auto origin = URL { URL { }, String::fromUTF8(documentURL) };
        snapshotRequire(origin.isValid() && origin.protocolIs("https"_s) && origin.user().isEmpty() && origin.password().isEmpty() && !origin.hasFragmentIdentifier()
            && utf8String(origin.string()) == m_documentURL, "snapshot document URL must be canonical credential-free HTTPS");
        snapshotRequire(root && *root == '/' && index && std::string(index) == std::string(root) + "/native-index.tsv", "index must be root/native-index.tsv");
        int rootFD = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        snapshotRequire(rootFD >= 0, "cannot open snapshot root directory");
        m_objectsFD = openat(rootFD, "objects", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        snapshotRequire(m_objectsFD >= 0, "cannot open snapshot objects directory");
        auto bytes = readSnapshotFD(openat(rootFD, "native-index.tsv", O_RDONLY | O_NOFOLLOW | O_CLOEXEC), maxSnapshotIndexBytes);
        snapshotRequire(!close(rootFD), "snapshot root close failed");
        m_indexSHA256 = nativeSHA256(bytes);
        m_indexFNV = fnv1a64(bytes);
        std::string text(bytes.begin(), bytes.end());
        snapshotRequire(!text.empty() && text.back() == '\n' && text.find('\r') == std::string::npos && text.find('\0') == std::string::npos, "invalid index framing");
        size_t end = text.find('\n');
        auto header = fields(std::string_view(text).substr(0, end));
        snapshotRequire(header.size() == 3 && header[0] == "SCARLET_SNAPSHOT_INDEX_V1", "unsupported snapshot index");
        auto expectedCount = decimal(header[1]);
        auto expectedBytes = decimal(header[2]);
        snapshotRequire(expectedCount && expectedCount <= maxSnapshotResources && expectedBytes <= maxSnapshotTotalBytes, "snapshot limits exceeded");
        uint64_t total = 0;
        for (size_t start = end + 1; start < text.size(); start = end + 1) {
            snapshotRequire(m_entries.size() < maxSnapshotResources, "snapshot entry count exceeds native limit");
            end = text.find('\n', start);
            snapshotRequire(end != std::string::npos, "unterminated index record");
            auto item = fields(std::string_view(text).substr(start, end - start));
            snapshotRequire(item.size() == 10 && item[9] == "200", "invalid index record or unsupported response status");
            auto url = URL { URL { }, String::fromUTF8(item[0].c_str()) };
            snapshotRequire(url.isValid() && url.protocolIs("https"_s) && url.user().isEmpty() && url.password().isEmpty() && !url.hasFragmentIdentifier(), "index requires credential-free HTTPS URLs without fragments");
            snapshotRequire(utf8String(url.string()) == item[0], "index URL is not WebCore canonical");
            snapshotRequire(url.host() == origin.host() && url.port() == origin.port(), "snapshot resources must have the original document's HTTPS origin");
            snapshotRequire(safeObjectPath(item[1]) && isHex(item[5], 64) && isHex(item[6], 16), "invalid snapshot path or hash");
            snapshotRequire(!item[2].empty() && item[2].size() <= 128 && item[2].find('/') != std::string::npos && item[2].find(';') == std::string::npos && item[2].find(' ') == std::string::npos, "invalid snapshot MIME");
            for (const auto& field : item) {
                for (unsigned char ch : field)
                    snapshotRequire(ch >= 32 && ch != 127, "control character in snapshot index");
            }
            auto count = decimal(item[4]);
            snapshotRequire(count <= maxSnapshotResourceBytes && total <= maxSnapshotTotalBytes - count, "snapshot byte limits exceeded");
            total += count;
            for (const auto& existing : m_entries)
                snapshotRequire(existing.url != item[0], "duplicate snapshot URL");
            m_entries.push_back({ item[0], item[1], item[2], item[3], static_cast<size_t>(count), item[5], item[6], item[7], item[8] });
        }
        snapshotRequire(m_entries.size() == expectedCount && total == expectedBytes, "snapshot index totals mismatch");
        snapshotRequire(lookup(origin), "exact snapshot document URL is absent from index");
        m_totalBytes = total;
        fprintf(stdout, "SCARLET_SNAPSHOT_INDEX resources=%zu bytes=%llu\n", m_entries.size(), static_cast<unsigned long long>(total));
    }

    const SnapshotEntry* lookup(const URL& requestURL) const
    {
        auto url = requestURL;
        // HTTP fragments select an SVG element but are never transport bytes.
        url.removeFragmentIdentifier();
        auto key = utf8String(url.string());
        for (const auto& entry : m_entries) {
            if (entry.url == key)
                return &entry;
        }
        return nullptr;
    }

    Ref<SharedBuffer> read(const SnapshotEntry& entry) const
    {
        auto bytes = readSnapshotFD(openat(m_objectsFD, entry.path.c_str() + 8, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), maxSnapshotResourceBytes);
        snapshotRequire(bytes.size() == entry.bytes, "snapshot size mismatch");
        snapshotRequire(nativeSHA256(bytes) == entry.sha256 && fnv1a64(bytes) == entry.fnv, "snapshot hash mismatch");
        return SharedBuffer::create(std::span<const uint8_t> { bytes.data(), bytes.size() });
    }

    const std::string& indexSHA256() const { return m_indexSHA256; }
    const std::string& indexFNV() const { return m_indexFNV; }
    const std::string& documentURL() const { return m_documentURL; }
    size_t entryCount() const { return m_entries.size(); }
    uint64_t totalBytes() const { return m_totalBytes; }

private:
    int m_objectsFD { -1 };
    std::vector<SnapshotEntry> m_entries;
    std::string m_indexSHA256;
    std::string m_indexFNV;
    std::string m_documentURL;
    uint64_t m_totalBytes { 0 };
};

bool snapshotTypeAllowed(CachedResource::Type type)
{
    switch (type) {
    case CachedResource::Type::CSSStyleSheet:
    case CachedResource::Type::ImageResource:
    case CachedResource::Type::FontResource:
    case CachedResource::Type::SVGFontResource:
    case CachedResource::Type::SVGDocumentResource:
    case CachedResource::Type::Icon:
        return true;
    default:
        return false;
    }
}

class ScarletOfflineLoaderStrategy final : public LoaderStrategy {
public:
    ScarletOfflineLoaderStrategy() = default;
    ScarletOfflineLoaderStrategy(const char* root, const char* index, const char* documentURL)
        : m_snapshot(std::make_unique<SnapshotStore>(root, index, documentURL))
    {
    }

    void loadResource(LocalFrame& frame, CachedResource& resource, ResourceRequest&& request,
        const ResourceLoaderOptions& options, CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& completion) final
    {
        if (m_snapshot) {
            snapshotRequire(m_records.size() < maxSnapshotRequests, "snapshot request limit exceeded");
            auto* entry = m_snapshot->lookup(request.url());
            bool allowed = entry && snapshotTypeAllowed(resource.type()) && request.httpMethod() == "GET"_s;
            auto recordIndex = m_records.size();
            m_records.push_back({ utf8String(request.url().string()), entry, &resource, allowed ? "initializing" : "blocked" });
            if (!allowed) {
                fprintf(stdout, "SCARLET_SNAPSHOT_RESOURCE outcome=blocked url=%s reason=%s\n", m_records.back().url.c_str(), entry ? "resource-type-or-method" : "URL-not-in-index");
                resource.setResourceError(blockedError(request));
                completion(nullptr);
                return;
            }
            ++m_initializing;
            SubresourceLoader::create(frame, resource, WTF::move(request), options,
                [this, entry, recordIndex, completion = WTF::move(completion)](RefPtr<SubresourceLoader>&& loader) mutable {
                    snapshotRequire(m_initializing, "snapshot initialization accounting underflow");
                    --m_initializing;
                    if (!loader) {
                        m_records[recordIndex].outcome = "failed";
                        fprintf(stdout, "SCARLET_SNAPSHOT_RESOURCE outcome=failed url=%s reason=WebCore-initialization\n", m_records[recordIndex].url.c_str());
                        completion(nullptr);
                        return;
                    }
                    m_records[recordIndex].outcome = "queued";
                    snapshotRequire(m_active.emplace(loader.get(), recordIndex).second, "duplicate active snapshot loader");
                    // CachedResource must receive and retain its actual loader
                    // before any response/data/completion can be delivered.
                    m_pending.push_back({ loader, entry, recordIndex });
                    completion(WTF::move(loader));
                });
            return;
        }
        // CachedResource::load's nullptr branch calls failBeforeStarting().
        // Supply a real failure before completing; never create a transport.
        resource.setResourceError(blockedError(request));
        completion(nullptr);
    }

    void loadResourceSynchronously(FrameLoader&, ResourceLoaderIdentifier, const ResourceRequest& request,
        ClientCredentialPolicy, const FetchOptions&, const HTTPHeaderMap&, ResourceError& error,
        ResourceResponse& response, Vector<uint8_t>& data) final
    {
        data.clear();
        response = ResourceResponse { };
        error = blockedError(request);
    }

    bool startKeepAliveLoadForWebKitLegacy(FrameLoader&, const ResourceRequest& request,
        const ResourceLoaderOptions&, CompletionHandler<void(const ResourceError&, const ResourceResponse&)>&& completion) final
    {
        // True means this strategy consumed/completed the request, not success.
        auto error = blockedError(request);
        ResourceResponse response;
        completion(error, response);
        return true;
    }

    void preconnectTo(FrameLoader&, ResourceRequest&& request, StoredCredentialsPolicy,
        ShouldPreconnectAsFirstParty, PreconnectCompletionHandler&& completion) final
    {
        completion(blockedError(request));
    }

    // The default profile has no transport work. Snapshot resources are a
    // bounded single-thread queue, drained by the explicit runner load pump.
    // Upstream resource priority is not a correctness condition for this local
    // substitute transport; no parallel network scheduler is invented.
    void pageLoadCompleted(Page&) final { }
    void browsingContextRemoved(LocalFrame&) final { }
    void servePendingRequests(ResourceLoadPriority) final { }
    void suspendPendingRequests() final { if (m_snapshot) ++m_suspended; }
    void resumePendingRequests() final
    {
        if (m_snapshot) {
            snapshotRequire(m_suspended, "resume without snapshot suspension");
            --m_suspended;
        }
    }
    void setResourceLoadSchedulingMode(Page&, LoadSchedulingMode) final { }
    void prioritizeResourceLoads(const Vector<Ref<SubresourceLoader>>&) final { }
    void setCaptureExtraNetworkLoadMetricsEnabled(bool) final { }

    void remove(ResourceLoader* loader) final
    {
        if (!m_snapshot)
            unsupported("remove an unowned ResourceLoader");
        auto found = m_active.find(loader);
        if (found == m_active.end()) {
            // ResourceLoader::init can release itself on a real security-policy
            // failure before SubresourceLoader::create returns its pointer.
            snapshotRequire(m_initializing && loader->reachedTerminalState(), "remove an unowned snapshot ResourceLoader");
            return;
        }
        auto& record = m_records[found->second];
        if (record.resource->resourceError().isCancellation())
            record.outcome = "cancelled";
        else if (record.resource->errorOccurred())
            record.outcome = "failed";
        else {
            snapshotRequire(record.outcome == "delivering" && record.resource->status() == CachedResource::Cached, "snapshot terminated before real CachedResource completion");
            record.outcome = "loaded";
        }
        fprintf(stdout, "SCARLET_SNAPSHOT_RESOURCE outcome=%s url=%s mime=%s bytes=%zu sha256=%s fnv1a64=%s\n",
            record.outcome.c_str(), record.url.c_str(), record.entry->mime.c_str(), record.entry->bytes, record.entry->sha256.c_str(), record.entry->fnv.c_str());
        m_active.erase(found);
    }
    void setDefersLoading(ResourceLoader& loader, bool) final
    {
        if (!m_snapshot)
            unsupported("defer an unowned ResourceLoader");
        snapshotRequire(m_active.contains(&loader) || m_initializing, "defer an unowned snapshot ResourceLoader");
        // ResourceLoader updates its real flag before this callback. The next
        // pump consults that flag and preserves the pending response bytes.
    }
    void crossOriginRedirectReceived(ResourceLoader*, const URL&) final { unsupported("redirect an unowned ResourceLoader"); }

    bool isOnLine() const final { return false; }
    void addOnlineStateChangeListener(Function<void(bool)>&& listener) final
    {
        // Registration does not invent a state transition. Retain observers;
        // this strategy's permanent offline state cannot change later.
        m_onlineStateListeners.append(WTF::move(listener));
    }

    void isResourceLoadFinished(CachedResource& resource, CompletionHandler<void(bool)>&& completion) final
    {
        // Matches the upstream WebResourceLoadScheduler's no-loader contract.
        // A rejected attempt is finished even though no data was delivered.
        completion(!resource.loader() || (m_snapshot && !m_active.contains(resource.loader())));
    }

    ResourceError cancelledError(const ResourceRequest& request) const final
    {
        return offlineError(OfflineError::Cancelled, request.url(), ResourceError::Type::Cancellation);
    }
    ResourceError blockedError(const ResourceRequest& request) const final
    {
        return offlineError(OfflineError::Blocked, request.url());
    }
    bool isBlockedError(const ResourceError& error) const final
    {
        return hasOfflineCode(error, OfflineError::Blocked)
            || hasOfflineCode(error, OfflineError::ContentBlocker)
            || hasOfflineCode(error, OfflineError::ContentFilter);
    }
    ResourceError blockedByContentBlockerError(const ResourceRequest& request) const final
    {
        return offlineError(OfflineError::ContentBlocker, request.url());
    }
    ResourceError cannotShowURLError(const ResourceRequest& request) const final
    {
        return offlineError(OfflineError::CannotShowURL, request.url());
    }
    ResourceError interruptedForPolicyChangeError(const ResourceRequest& request) const final
    {
        return offlineError(OfflineError::PolicyChange, request.url(), ResourceError::Type::Cancellation);
    }
#if ENABLE(CONTENT_FILTERING)
    ResourceError blockedByContentFilterError(const ResourceRequest& request) const final
    {
        return offlineError(OfflineError::ContentFilter, request.url());
    }
#endif
    ResourceError cannotShowMIMETypeError(const ResourceResponse& response) const final
    {
        return offlineError(OfflineError::CannotShowMIMEType, response.url());
    }
    ResourceError fileDoesNotExistError(const ResourceResponse& response) const final
    {
        return offlineError(OfflineError::FileUnavailable, response.url());
    }
    ResourceError httpsUpgradeRedirectLoopError(const ResourceRequest& request) const final
    {
        return offlineError(OfflineError::HTTPSRedirectLoop, request.url());
    }
    ResourceError httpNavigationWithHTTPSOnlyError(const ResourceRequest& request) const final
    {
        return offlineError(OfflineError::HTTPSOnly, request.url());
    }
    bool isHttpNavigationWithHTTPSOnlyError(const ResourceError& error) const final
    {
        return hasOfflineCode(error, OfflineError::HTTPSOnly);
    }
    ResourceError pluginWillHandleLoadError(const ResourceResponse& response) const final
    {
        return offlineError(OfflineError::PluginUnavailable, response.url());
    }

    unsigned pump()
    {
        snapshotRequire(m_snapshot && WTF::isMainThread(), "snapshot pump outside initialized native main thread");
        if (m_suspended || m_pumping)
            return 0;
        m_pumping = true;
        auto pending = std::move(m_pending);
        m_pending.clear();
        unsigned delivered = 0;
        for (auto& item : pending) {
            if (item.loader->reachedTerminalState())
                continue;
            if (item.loader->defersLoading()) {
                m_pending.push_back(std::move(item));
                continue;
            }
            snapshotRequire(m_deliveredBytes <= maxSnapshotTotalBytes - item.entry->bytes, "snapshot cumulative delivered byte limit exceeded");
            m_deliveredBytes += item.entry->bytes;
            auto buffer = m_snapshot->read(*item.entry);
            ResourceResponse response { URL { URL { }, String::fromUTF8(item.entry->url.c_str()) },
                String::fromUTF8(item.entry->mime.c_str()), static_cast<long long>(item.entry->bytes), String::fromUTF8(item.entry->charset.c_str()) };
            response.setSource(ResourceResponse::Source::DiskCache);
            response.setHTTPStatusCode(200);
            response.setHTTPStatusText("OK"_s);
            auto contentType = item.entry->mime + (item.entry->charset.empty() ? "" : "; charset=" + item.entry->charset);
            response.setHTTPHeaderField(HTTPHeaderName::ContentType, String::fromUTF8(contentType.c_str()));
            response.setHTTPHeaderField(HTTPHeaderName::ContentLength, String::fromUTF8(std::to_string(item.entry->bytes).c_str()));
            if (!item.entry->allowOrigin.empty())
                response.setHTTPHeaderField(HTTPHeaderName::AccessControlAllowOrigin, String::fromUTF8(item.entry->allowOrigin.c_str()));
            if (!item.entry->resourcePolicy.empty())
                response.setHTTPHeaderField(HTTPHeaderName::CrossOriginResourcePolicy, String::fromUTF8(item.entry->resourcePolicy.c_str()));
            m_records[item.recordIndex].outcome = "delivering";
            // This is the upstream archive/substitute-resource delivery path.
            // Policy and CachedResource callbacks perform the actual CSS/font/
            // image decoding, including SRI/CORS/MIME rejection if applicable.
            item.loader->deliverResponseAndData(WTF::move(response), RefPtr<FragmentedSharedBuffer> { WTF::move(buffer) });
            ++delivered;
        }
        m_pumping = false;
        return delivered;
    }

    ScarletSnapshotResourceState state() const
    {
        ScarletSnapshotResourceState state;
        state.requests = static_cast<uint32_t>(m_records.size());
        state.pending = static_cast<uint32_t>(m_active.size() + m_initializing);
        for (const auto& record : m_records) {
            auto outcome = finalOutcome(record);
            if (outcome == "loaded") {
                ++state.loaded;
                state.loadedBytes += record.entry->bytes;
            } else if (outcome == "blocked")
                ++state.blocked;
            else if (outcome == "failed")
                ++state.failed;
            else if (outcome == "cancelled")
                ++state.cancelled;
        }
        return state;
    }

    std::string evidenceJSON() const
    {
        auto current = state();
        snapshotRequire(!!m_snapshot, "snapshot evidence requested from offline profile");
        std::string json = "{\"transport\":\"exact-https-snapshot-bytes\",\"index_sha256\":" + jsonQuoted(m_snapshot->indexSHA256())
            + ",\"index_fnv1a64\":" + jsonQuoted(m_snapshot->indexFNV())
            + ",\"document_url\":" + jsonQuoted(m_snapshot->documentURL())
            + ",\"index_resources\":" + std::to_string(m_snapshot->entryCount()) + ",\"index_bytes\":" + std::to_string(m_snapshot->totalBytes())
            + ",\"requests\":" + std::to_string(current.requests)
            + ",\"loaded\":" + std::to_string(current.loaded) + ",\"loaded_bytes\":" + std::to_string(current.loadedBytes)
            + ",\"blocked\":" + std::to_string(current.blocked) + ",\"failed\":" + std::to_string(current.failed)
            + ",\"cancelled\":" + std::to_string(current.cancelled) + ",\"pending\":" + std::to_string(current.pending) + ",\"resources\":[";
        bool first = true;
        for (const auto& record : m_records) {
            if (!first)
                json += ',';
            first = false;
            json += "{\"url\":" + jsonQuoted(record.url) + ",\"outcome\":" + jsonQuoted(finalOutcome(record));
            if (record.entry)
                json += ",\"mime\":" + jsonQuoted(record.entry->mime) + ",\"bytes\":" + std::to_string(record.entry->bytes)
                    + ",\"sha256\":" + jsonQuoted(record.entry->sha256) + ",\"fnv1a64\":" + jsonQuoted(record.entry->fnv);
            json += '}';
        }
        return json + "]}";
    }

private:
    struct Record {
        std::string url;
        const SnapshotEntry* entry;
        RefPtr<CachedResource> resource;
        std::string outcome;
    };
    struct Pending {
        RefPtr<SubresourceLoader> loader;
        const SnapshotEntry* entry;
        size_t recordIndex;
    };
    static std::string_view finalOutcome(const Record& record)
    {
        // A font/image may detect a decode error only when layout first uses
        // it. Retain the real CachedResource to report that later failure.
        if (record.outcome == "loaded" && record.resource->errorOccurred())
            return "failed";
        return record.outcome;
    }
    std::unique_ptr<SnapshotStore> m_snapshot;
    std::vector<Record> m_records;
    std::vector<Pending> m_pending;
    std::map<ResourceLoader*, size_t> m_active;
    unsigned m_initializing { 0 };
    unsigned m_suspended { 0 };
    uint64_t m_deliveredBytes { 0 };
    bool m_pumping { false };
    Vector<Function<void(bool)>> m_onlineStateListeners;
};

class ScarletOfflinePlatformStrategies final : public PlatformStrategies {
public:
    ScarletOfflinePlatformStrategies()
        : m_loader(new ScarletOfflineLoaderStrategy)
    {
    }
    ScarletOfflinePlatformStrategies(const char* root, const char* index, const char* documentURL)
        : m_loader(new ScarletOfflineLoaderStrategy(root, index, documentURL))
    {
    }

    ScarletOfflineLoaderStrategy& snapshotLoader() { return *m_loader; }

private:
    LoaderStrategy* createLoaderStrategy() final { return m_loader; }
    PasteboardStrategy* createPasteboardStrategy() final { unsupported("clipboard"); }
    MediaStrategy* createMediaStrategy() final { unsupported("media"); }
    BlobRegistry* createBlobRegistry() final { unsupported("blob registry"); }
#if ENABLE(DECLARATIVE_WEB_PUSH)
    PushStrategy* createPushStrategy() final { unsupported("web push"); }
#endif

    // PlatformStrategies stores CheckedPtrs. The observer target must have
    // its own heap allocation and outlive every Page and retained callback.
    ScarletOfflineLoaderStrategy* const m_loader;
};

ScarletOfflinePlatformStrategies* snapshotStrategies { nullptr };

} // namespace

void initializeScarletOfflineStrategies()
{
    RELEASE_ASSERT(WTF::isMainThread());
    static auto* strategies = new ScarletOfflinePlatformStrategies;
    setPlatformStrategies(strategies);
    RELEASE_ASSERT(platformStrategies() == strategies);
}

void initializeScarletSnapshotStrategies(const char* root, const char* index, const char* documentURL)
{
    RELEASE_ASSERT(WTF::isMainThread());
    snapshotRequire(!snapshotStrategies, "snapshot strategy initialized twice");
    snapshotStrategies = new ScarletOfflinePlatformStrategies(root, index, documentURL);
    setPlatformStrategies(snapshotStrategies);
    RELEASE_ASSERT(platformStrategies() == snapshotStrategies);
}

unsigned pumpScarletSnapshotResources()
{
    snapshotRequire(snapshotStrategies, "snapshot profile is not initialized");
    return snapshotStrategies->snapshotLoader().pump();
}

ScarletSnapshotResourceState scarletSnapshotResourceState()
{
    snapshotRequire(snapshotStrategies, "snapshot profile is not initialized");
    return snapshotStrategies->snapshotLoader().state();
}

std::string scarletSnapshotResourceEvidenceJSON()
{
    snapshotRequire(snapshotStrategies, "snapshot profile is not initialized");
    return snapshotStrategies->snapshotLoader().evidenceJSON();
}

} // namespace WebCore
