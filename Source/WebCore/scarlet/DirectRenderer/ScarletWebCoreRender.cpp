// Direct WebCore offline CPU renderer. All page parsing/layout/paint is WebCore.
// Built inside the Scarlet WebCore CMake tree; not a WPE/WebKit browser API.
#include "config.h"

#include "Color.h"
#include "CSSFontSelector.h"
#include "Document.h"
#include "DocumentLoader.h"
#include "DocumentResourceLoader.h"
#include "DocumentView.h"
#include "DocumentWriter.h"
#include "Element.h"
#include "EmptyClients.h"
#include "FrameLoader.h"
#include "FontFaceSet.h"
#include "GraphicsContextCairo.h"
#include "HTMLBodyElement.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "LocalFrameView.h"
#include "Page.h"
#include "PageConfiguration.h"
#include "ProcessWarming.h"
#include "RefPtrCairo.h"
#include "RenderTreeAsText.h"
#include "ScarletPlatform.h"
#include "ScarletOfflineStrategies.h"
#include "ScarletElementInspection.h"
#include "ScarletScriptEntryCounter.h"
#include "SandboxFlags.h"
#include "ScriptController.h"
#include "ScrollbarMode.h"
#include "Settings.h"
#include "ThreadGlobalData.h"
#include "ThreadTimers.h"
#include "StyleDocumentScope.h"
#include <pal/SessionID.h>
#include <wtf/MainThread.h>
#include <wtf/RunLoop.h>
#include <wtf/URL.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>
#include <cairo.h>
#include <fontconfig/fontconfig.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <span>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if !PLATFORM(SCARLET) || !OS(SCARLET) || !USE(CAIRO)
#error "The runner requires the real native Scarlet Cairo WebCore configuration."
#endif

using namespace WebCore;
using namespace WTF::StringLiterals;

namespace {
constexpr size_t maxInputBytes = 1024 * 1024;
constexpr unsigned maxDimension = 4096;
constexpr size_t maxOutputBytes = 64 * 1024 * 1024;
constexpr size_t maxMetadataBytes = 8 * 1024 * 1024;
constexpr const char* sourceRevision = "7024b36074b0584a72f9d91406ed3b13548fb4e0";

// A JSON commit plus the matching run-specific PASS log is the validity marker.
// Failed runs remove their temporary files; final image files alone never prove
// success. The exclusive prefix lock also prevents interleaved publication.
std::string outputLock;
std::vector<std::string> temporaryOutputs;

void cleanTransaction()
{
    for (auto& path : temporaryOutputs)
        unlink(path.c_str());
    if (!outputLock.empty())
        unlink(outputLock.c_str());
}

[[noreturn]] void fail(const char* operation)
{
    fprintf(stderr, "SCARLET_WEBCORE_ERROR %s (errno=%d)\n", operation, errno);
    exit(1);
}

void require(bool condition, const char* operation)
{
    if (!condition)
        fail(operation);
}

// This deterministic byte hash is a correlation checksum, not a security hash.
uint64_t fnv1a64(std::span<const unsigned char> bytes)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (auto byte : bytes) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::string hexHash(std::span<const unsigned char> bytes)
{
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016" PRIx64, fnv1a64(bytes));
    return buffer;
}

std::span<const unsigned char> bytesOf(const std::string& value)
{
    return { reinterpret_cast<const unsigned char*>(value.data()), value.size() };
}

std::string utf8(const String& string)
{
    auto bytes = string.utf8();
    return { reinterpret_cast<const char*>(bytes.data()), bytes.length() };
}

std::string jsonString(const std::string& value)
{
    require(!String::fromUTF8(std::span<const char>(value.data(), value.size())).isNull(), "metadata string is not valid UTF-8");
    std::string result = "\"";
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            result += '\\';
            result += static_cast<char>(byte);
        } else if (byte < 0x20) {
            char escaped[7];
            snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
            result += escaped;
        } else
            result += static_cast<char>(byte);
    }
    result += '"';
    return result;
}

std::string number(double value)
{
    require(std::isfinite(value), "selected layout geometry is non-finite");
    char buffer[64];
    auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 6);
    require(converted.ec == std::errc { } && converted.ptr > buffer && converted.ptr <= buffer + sizeof(buffer), "serialize layout geometry with native to_chars");
    return { buffer, converted.ptr };
}

std::string rectJSON(const FloatRect& rect)
{
    return "{\"x\":" + number(rect.x()) + ",\"y\":" + number(rect.y())
        + ",\"width\":" + number(rect.width()) + ",\"height\":" + number(rect.height()) + "}";
}

struct Options {
    std::string input;
    std::string prefix = "/tmp/webcore-frame";
    std::string runID;
    std::string fontConfig = "/etc/fonts/fonts.conf";
    unsigned width = 800;
    unsigned height = 600;
    std::vector<std::string> inspectIDs;
    std::vector<std::pair<std::string, std::string>> inspectSelectors;
    bool inspectBefore { false };
    uint32_t beforeGlyphCodepoint { 0 };
    std::vector<std::string> beforeFontSourceURLs;
    std::string resourceProfile;
    std::string documentURL;
    std::string snapshotResourceRoot;
    std::string snapshotIndex;
};

void teardownCheckpoint(const Options& options, const char* stage)
{
    require(printf("SCARLET_WEBCORE_TEARDOWN run_id=%s stage=%s\n", options.runID.c_str(), stage) >= 0, "write teardown checkpoint");
}

unsigned dimension(const char* begin, const char** end)
{
    require(*begin >= '0' && *begin <= '9', "invalid viewport size");
    char* parsedEnd = nullptr;
    errno = 0;
    auto result = strtoul(begin, &parsedEnd, 10);
    require(!errno && result > 0 && result <= maxDimension, "viewport dimension must be 1..4096");
    *end = parsedEnd;
    return static_cast<unsigned>(result);
}

Options parseOptions(int argc, char** argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("scarlet-webcore-render HTML --run-id TOKEN [--size WIDTHxHEIGHT] [--output-prefix PATH]\n"
             "  [--font-config PATH] [--inspect ELEMENT_ID] (at most 16 IDs)\n"
             "  [--inspect-selector ALIAS=CSS_SELECTOR] (at most 32 selectors)\n"
             "  [--inspect-before] [--inspect-before-glyph HEX] [--inspect-before-font-source HTTPS_URL]\n"
             "  [--resource-profile snapshotresources --document-url HTTPS_URL\n"
             "   --snapshot-resource-root PATH --snapshot-index PATH]\n"
             "Offline, script-disabled WebCore; UTF-8 input <= 1 MiB; viewport <= 4096x4096.\n"
             "TOKEN: unique ASCII letters/digits/_/-, 1..96 bytes. PATH: absolute ASCII path.\n"
             "Writes PATH.bgra, PATH.png, PATH.render-tree.txt and commits PATH.json last.\n"
             "Defaults: 800x600, /tmp/webcore-frame, /etc/fonts/fonts.conf.");
        exit(0);
    }
    require(argc >= 2 && argv[1][0] != '-', "expected local UTF-8 HTML path; use --help");
    Options options;
    options.input = argv[1];
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--inspect-before")) {
            options.inspectBefore = true;
            continue;
        }
        require(i + 1 < argc, "option requires a value");
        const char* flag = argv[i];
        const char* value = argv[++i];
        if (!strcmp(flag, "--size")) {
            const char* end = nullptr;
            options.width = dimension(value, &end);
            require(*end == 'x', "viewport syntax must be WIDTHxHEIGHT");
            options.height = dimension(end + 1, &end);
            require(!*end, "trailing characters in viewport size");
        } else if (!strcmp(flag, "--output-prefix"))
            options.prefix = value;
        else if (!strcmp(flag, "--run-id"))
            options.runID = value;
        else if (!strcmp(flag, "--font-config"))
            options.fontConfig = value;
        else if (!strcmp(flag, "--resource-profile"))
            options.resourceProfile = value;
        else if (!strcmp(flag, "--document-url"))
            options.documentURL = value;
        else if (!strcmp(flag, "--snapshot-resource-root"))
            options.snapshotResourceRoot = value;
        else if (!strcmp(flag, "--snapshot-index"))
            options.snapshotIndex = value;
        else if (!strcmp(flag, "--inspect-before-glyph")) {
            char* end = nullptr;
            errno = 0;
            auto scalar = strtoul(value, &end, 16);
            require(!errno && end != value && !*end && scalar && scalar <= 0x10ffff && !(scalar >= 0xd800 && scalar <= 0xdfff), "invalid glyph Unicode scalar");
            options.beforeGlyphCodepoint = static_cast<uint32_t>(scalar);
            options.inspectBefore = true;
        } else if (!strcmp(flag, "--inspect-before-font-source")) {
            require(options.beforeFontSourceURLs.size() < 16 && strlen(value) <= 2048, "too many or invalid glyph font sources");
            auto url = URL { String::fromUTF8(value) };
            require(url.isValid() && url.protocolIs("https"_s), "glyph font source must be an absolute HTTPS URL");
            options.beforeFontSourceURLs.emplace_back(value);
        } else if (!strcmp(flag, "--inspect-selector")) {
            require(options.inspectSelectors.size() < 32 && strlen(value) <= 2048, "too many or invalid selectors");
            std::string argument { value };
            auto equal = argument.find('=');
            require(equal != std::string::npos && equal && equal <= 96 && equal + 1 < argument.size(), "selector syntax must be ALIAS=CSS_SELECTOR");
            auto alias = argument.substr(0, equal);
            auto selector = argument.substr(equal + 1);
            for (unsigned char byte : alias)
                require((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_' || byte == '-', "invalid selector alias");
            require(!String::fromUTF8(selector.c_str()).isNull(), "selector is not valid UTF-8");
            for (auto& existing : options.inspectSelectors)
                require(existing.first != alias, "duplicate selector alias");
            options.inspectSelectors.emplace_back(WTF::move(alias), WTF::move(selector));
        }
        else if (!strcmp(flag, "--inspect")) {
            require(options.inspectIDs.size() < 16 && *value && strlen(value) <= 128, "invalid inspection ID");
            require(!String::fromUTF8(value).isNull(), "inspection ID is not valid UTF-8");
            for (auto& existing : options.inspectIDs)
                require(existing != value, "duplicate inspection ID");
            options.inspectIDs.emplace_back(value);
        } else
            fail("unknown option");
    }
    require(!options.prefix.empty() && options.prefix.size() <= 512, "invalid output prefix");
    require(options.input.size() <= 512 && !String::fromUTF8(options.input.c_str()).isNull(), "input path is too long or not valid UTF-8");
    require(options.prefix[0] == '/' && options.prefix.back() != '/', "output prefix must be an absolute file path");
    for (unsigned char byte : options.prefix)
        require((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')
            || byte == '/' || byte == '.' || byte == '_' || byte == '-', "output prefix contains unsupported path characters");
    size_t componentBegin = 1;
    while (componentBegin < options.prefix.size()) {
        auto componentEnd = options.prefix.find('/', componentBegin);
        auto component = options.prefix.substr(componentBegin, componentEnd - componentBegin);
        require(!component.empty() && component != "." && component != "..", "output prefix has a noncanonical path component");
        if (componentEnd == std::string::npos)
            break;
        componentBegin = componentEnd + 1;
    }
    require(!options.runID.empty() && options.runID.size() <= 96, "required unique --run-id TOKEN is missing or too long");
    for (unsigned char byte : options.runID)
        require((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')
            || byte == '_' || byte == '-', "run ID contains unsupported characters");
    require(!options.fontConfig.empty() && options.fontConfig.size() <= 512, "invalid font configuration path");
    require(!String::fromUTF8(options.fontConfig.c_str()).isNull(), "font configuration path is not valid UTF-8");
    require(options.resourceProfile.empty() || options.resourceProfile == "snapshotresources", "unsupported resource profile");
    if (options.resourceProfile.empty())
        require(options.documentURL.empty() && options.snapshotResourceRoot.empty() && options.snapshotIndex.empty(), "snapshot arguments require explicit snapshotresources profile");
    else {
        auto url = URL { String::fromUTF8(options.documentURL.c_str()) };
        require(options.documentURL.size() <= 2048 && url.isValid() && url.protocolIs("https"_s), "snapshot document must be an absolute HTTPS URL");
        for (auto* path : { &options.snapshotResourceRoot, &options.snapshotIndex }) {
            require(!path->empty() && path->size() <= 512 && path->front() == '/' && path->back() != '/', "snapshot path must be absolute and canonical");
            for (unsigned char byte : *path)
                require((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '/' || byte == '.' || byte == '_' || byte == '-', "snapshot path contains unsupported characters");
            size_t begin = 1;
            while (begin < path->size()) {
                auto end = path->find('/', begin);
                auto component = path->substr(begin, end - begin);
                require(!component.empty() && component != "." && component != "..", "snapshot path has a noncanonical component");
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
        }
        require(options.snapshotIndex == options.snapshotResourceRoot + "/native-index.tsv", "snapshot index must belong to its resource root");
    }
    for (const char* suffix : { ".bgra", ".png", ".render-tree.txt", ".json", ".lock" })
        require(options.input != options.prefix + suffix, "output would overwrite input");
    if (options.inspectIDs.empty())
        options.inspectIDs = { "probe", "flow", "label", "tile-a", "script-marker" };
    return options;
}

std::string readFile(const std::string& path, size_t limit)
{
    FILE* file = fopen(path.c_str(), "rb");
    require(file, "open input or PNG evidence");
    struct stat opened { };
    require(!fstat(fileno(file), &opened) && S_ISREG(opened.st_mode), "input or PNG evidence must be a regular file");
    require(opened.st_size >= 0 && static_cast<uint64_t>(opened.st_size) <= limit, "input or evidence exceeds byte limit");
    std::string input;
    char buffer[8192];
    while (size_t count = fread(buffer, 1, sizeof(buffer), file)) {
        require(input.size() + count <= limit, "input or evidence exceeds byte limit");
        input.append(buffer, count);
    }
    require(!ferror(file), "read input");
    require(!fclose(file), "close input");
    require(!input.empty(), "input or PNG evidence is empty");
    return input;
}

void beginTransaction(const Options& options)
{
    // Protect an input/font file named through a symlink or hard-link alias.
    struct stat inputStat { }, fontStat { };
    bool haveInput = !stat(options.input.c_str(), &inputStat);
    bool haveFont = !stat(options.fontConfig.c_str(), &fontStat);
    for (const char* suffix : { ".bgra", ".png", ".render-tree.txt", ".json", ".lock" }) {
        struct stat outputStat { };
        if (!stat((options.prefix + suffix).c_str(), &outputStat)) {
            require(!haveInput || inputStat.st_dev != outputStat.st_dev || inputStat.st_ino != outputStat.st_ino, "output aliases HTML input");
            require(!haveFont || fontStat.st_dev != outputStat.st_dev || fontStat.st_ino != outputStat.st_ino, "output aliases font configuration");
        }
    }
    auto lockPath = options.prefix + ".lock";
    int lock = open(lockPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(lock >= 0, "acquire exclusive output prefix lock (remove only a confirmed stale lock)");
    outputLock = lockPath;
    require(!atexit(cleanTransaction), "register output cleanup");
    require(!close(lock), "close output prefix lock");
    // Clear the previous commit before input/font validation can fail. A crash
    // may leave old images, but cannot leave valid metadata for this attempt.
    require(!unlink((options.prefix + ".json").c_str()) || errno == ENOENT, "invalidate previous metadata");
    for (const char* suffix : { ".bgra", ".png", ".render-tree.txt", ".json" }) {
        auto temporary = options.prefix + "." + options.runID + suffix + ".tmp";
        struct stat oldTemporary { };
        require(lstat(temporary.c_str(), &oldTemporary) && errno == ENOENT, "run ID has stale temporary output; use a new ID");
        temporaryOutputs.push_back(WTF::move(temporary));
    }
}

const std::string& temporaryOutput(size_t index)
{
    return temporaryOutputs[index];
}

void publishOutput(size_t index, const std::string& path)
{
    require(!rename(temporaryOutput(index).c_str(), path.c_str()), "publish output artifact");
}

void writeFile(const std::string& path, std::span<const unsigned char> bytes)
{
    FILE* file = fopen(path.c_str(), "wb");
    require(file, "open output");
    size_t written = fwrite(bytes.data(), 1, bytes.size(), file);
    bool good = written == bytes.size() && !ferror(file);
    int closed = fclose(file);
    require(good && !closed, "write or close output");
}

std::string initializeFonts(const std::string& configurationPath)
{
    // Load exactly the native guest's supplied configuration, not host defaults.
    FcConfig* configuration = FcConfigCreate();
    require(configuration, "create Fontconfig configuration");
    require(FcConfigParseAndLoad(configuration, reinterpret_cast<const FcChar8*>(configurationPath.c_str()), FcTrue), "load Fontconfig configuration");
    require(FcConfigBuildFonts(configuration), "scan native fonts");
    require(FcConfigSetCurrent(configuration), "activate Fontconfig configuration");
    FcConfigDestroy(configuration); // FcConfigSetCurrent keeps its own reference.
    std::string evidence = "[";
    unsigned index = 0;
    for (const char* family : { "serif", "sans-serif", "monospace" }) {
        // Family names are literal strings, not Fontconfig pattern syntax.
        // FcNameParse treats '-' as the family/size separator; that would turn
        // "sans-serif" into "sans" and misreport the real WebCore font match.
        FcPattern* requested = FcPatternCreate();
        require(requested, "create font pattern");
        require(FcPatternAddString(requested, FC_FAMILY, reinterpret_cast<const FcChar8*>(family)), "set literal font family");
        require(FcConfigSubstitute(nullptr, requested, FcMatchPattern), "substitute font pattern");
        FcDefaultSubstitute(requested);
        FcResult result;
        FcPattern* match = FcFontMatch(nullptr, requested, &result);
        require(match && result == FcResultMatch, "resolve native font");
        FcChar8* file = nullptr;
        FcChar8* matchedFamily = nullptr;
        require(FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch, "resolve font file");
        require(FcPatternGetString(match, FC_FAMILY, 0, &matchedFamily) == FcResultMatch, "resolve font family");
        FILE* readable = fopen(reinterpret_cast<const char*>(file), "rb");
        require(readable, "font file is not readable");
        require(!fclose(readable), "close font file");
        if (index++)
            evidence += ',';
        evidence += "{\"requested\":" + jsonString(family) + ",\"family\":"
            + jsonString(reinterpret_cast<const char*>(matchedFamily)) + ",\"file\":"
            + jsonString(reinterpret_cast<const char*>(file)) + "}";
        printf("SCARLET_FONT family=%s matched=%s file=%s\n", family, matchedFamily, file);
        FcPatternDestroy(match);
        FcPatternDestroy(requested);
    }
    return evidence + ']';
}

std::string settleSnapshot(Document& document, LocalFrameView& view, const Options& options)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 15 };
    unsigned stablePasses = 0;
    for (unsigned pass = 1; pass <= 20000; ++pass) {
        require(std::chrono::steady_clock::now() < deadline, "snapshot loading exceeded 15-second deadline");
        pumpScarletSnapshotResources();
        WTF::RunLoop::cycle();
        document.updateLayoutIgnorePendingStylesheets();
        // Layout discovers CSS fonts/images. Deliver through the real loader,
        // then cycle native timers/tasks and recheck actual completion/layout.
        pumpScarletSnapshotResources();
        WTF::RunLoop::cycle();
        document.updateLayoutIgnorePendingStylesheets();
        auto state = scarletSnapshotResourceState();
        require(!state.blocked && !state.failed && !state.cancelled, "snapshot resource delivery was blocked, failed or cancelled");
        auto* selector = document.fontSelectorIfExists();
        bool fontSetLoaded = !selector || selector->fontFaceSet().status() == FontFaceSet::LoadStatus::Loaded;
        bool pendingSheets = document.styleScope().hasPendingSheets();
        int pendingRequests = document.cachedResourceLoader().requestCount();
        require(pendingRequests >= 0, "snapshot native request count is negative");
        if (!pendingSheets && fontSetLoaded && !pendingRequests && !state.pending && !view.needsLayout())
            ++stablePasses;
        else
            stablePasses = 0;
        if (stablePasses == 3) {
            require(printf("SCARLET_SNAPSHOT_SETTLED run_id=%s passes=%u requests=%u loaded=%u bytes=%" PRIu64 " sheets_pending=false font_status=loaded native_requests=0 queued_resources=0\n",
                options.runID.c_str(), pass, state.requests, state.loaded, state.loadedBytes) >= 0, "write snapshot settlement log");
            return "{\"complete\":true,\"passes\":" + std::to_string(pass)
                + ",\"stylesheets_pending\":false,\"font_set_status\":\"loaded\",\"native_requests\":0,\"queued_resources\":0}";
        }
    }
    fail("snapshot loading exceeded bounded native RunLoop cycles");
}

std::string selectorsJSON(Document& document, const Options& options)
{
    std::vector<ScarletElementInspection> inspected;
    std::string error;
    bool success = inspectScarletElements(document, options.inspectSelectors, inspected, error,
        options.inspectBefore, options.beforeGlyphCodepoint, options.beforeFontSourceURLs);
    require(success, error.empty() ? "inspect real DOM selectors" : error.c_str());
    std::string json = "[";
    for (auto& item : inspected) {
        if (json.size() > 1)
            json += ',';
        json += "{\"alias\":" + jsonString(item.alias) + ",\"selector\":" + jsonString(item.selector)
            + ",\"found\":" + (item.found ? "true" : "false");
        if (item.found) {
            json += ",\"bounds\":{\"x\":" + number(item.x) + ",\"y\":" + number(item.y)
                + ",\"width\":" + number(item.width) + ",\"height\":" + number(item.height) + '}'
                + ",\"text\":" + jsonString(item.text) + ",\"display\":" + jsonString(item.display)
                + ",\"visibility\":" + jsonString(item.visibility) + ",\"font_family\":" + jsonString(item.fontFamily)
                + ",\"font_size\":" + jsonString(item.fontSize) + ",\"font_weight\":" + jsonString(item.fontWeight);
            if (item.isImage)
                json += ",\"image\":{\"complete\":" + std::string(item.imageComplete ? "true" : "false")
                    + ",\"natural_width\":" + std::to_string(item.naturalWidth) + ",\"natural_height\":" + std::to_string(item.naturalHeight)
                    + ",\"resource_present\":" + (item.imageResourcePresent ? "true" : "false")
                    + ",\"can_render\":" + (item.imageCanRender ? "true" : "false")
                    + ",\"resource_status\":" + jsonString(item.imageResourceStatus) + '}';
            if (item.beforeInspected) {
                json += ",\"before\":{\"content\":" + jsonString(item.beforeContent)
                    + ",\"font_family\":" + jsonString(item.beforeFontFamily) + ",\"font_size\":" + jsonString(item.beforeFontSize)
                    + ",\"font_weight\":" + jsonString(item.beforeFontWeight);
                if (item.beforeGlyphInspected)
                    json += ",\"glyph\":{\"codepoint\":" + std::to_string(item.beforeGlyphCodepoint)
                        + ",\"glyph\":" + std::to_string(item.beforeGlyph) + ",\"font_present\":" + (item.beforeGlyphFontPresent ? "true" : "false")
                        + ",\"origin\":" + jsonString(item.beforeGlyphOrigin) + ",\"interstitial\":" + (item.beforeGlyphInterstitial ? "true" : "false")
                        + ",\"family\":" + jsonString(item.beforeGlyphFamily) + ",\"source_url\":" + jsonString(item.beforeGlyphSourceURL)
                        + ",\"source_decoded\":" + (item.beforeGlyphSourceDecoded ? "true" : "false")
                        + ",\"source_face_matches\":" + (item.beforeGlyphSourceFaceMatches ? "true" : "false")
                        + ",\"pseudo_text\":" + jsonString(item.beforeGlyphPseudoText)
                        + ",\"contains_codepoint\":" + (item.beforeGlyphContainsCodepoint ? "true" : "false") + '}';
                json += '}';
            }
            require(printf("SCARLET_SELECTOR run_id=%s alias=%s rect=%s,%s,%s,%s display=%s visibility=%s\n", options.runID.c_str(), item.alias.c_str(),
                number(item.x).c_str(), number(item.y).c_str(), number(item.width).c_str(), number(item.height).c_str(), item.display.c_str(), item.visibility.c_str()) >= 0, "write selector geometry log");
        }
        json += '}';
    }
    return json + ']';
}
} // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    auto options = parseOptions(argc, argv);
    beginTransaction(options);
    auto input = readFile(options.input, maxInputBytes);
    auto html = String::fromUTF8(std::span<const char>(input.data(), input.size()));
    require(!html.isNull(), "input is not valid UTF-8");
    const uint32_t byteOrderProbe = 1;
    require(*reinterpret_cast<const unsigned char*>(&byteOrderProbe) == 1, "BGRA contract requires little endian");
    printf("SCARLET_WEBCORE_BEGIN run_id=%s revision=%s input_bytes=%zu viewport=%ux%u input_fnv1a64=%s output=%s\n",
        options.runID.c_str(), sourceRevision, input.size(), options.width, options.height, hexHash(bytesOf(input)).c_str(), options.prefix.c_str());

    auto fonts = initializeFonts(options.fontConfig);
    ScriptController::initializeMainThread();
    require(WTF::isMainThread(), "WebCore main thread initialization");
    bool snapshotResources = options.resourceProfile == "snapshotresources";
    if (snapshotResources)
        initializeScarletSnapshotStrategies(options.snapshotResourceRoot.c_str(), options.snapshotIndex.c_str(), options.documentURL.c_str());
    else
        initializeScarletOfflineStrategies();
    IntSize size { static_cast<int>(options.width), static_cast<int>(options.height) };
    FloatRect display { 0, 0, static_cast<float>(options.width), static_cast<float>(options.height) };
    setScarletScreenConfiguration(display, display);
    ProcessWarming::prewarmGlobally();

    auto configuration = pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    configuration.loadsSubresources = snapshotResources;
    if (snapshotResources) {
        auto* parameters = std::get_if<PageConfiguration::LocalMainFrameCreationParameters>(&configuration.mainFrameCreationParameters);
        require(parameters, "snapshot main frame must be local");
        // Retain the original HTTPS origin for same-origin CSS/fonts. Script
        // sandbox restrictions and the script=false setting remain enabled.
        parameters->effectiveSandboxFlags.remove(SandboxFlag::Origin);
    }
    RefPtr page = Page::create(WTF::move(configuration));
    page->settings().setScriptEnabled(false);
    if (snapshotResources) {
        // Empty-client pages default to manual image loading. This explicit
        // resource profile must load original indexed images as well as CSS
        // and fonts; enable the real image-loader settings before frame init.
        page->settings().setImagesEnabled(true);
        page->settings().setLoadsImagesAutomatically(true);
    }
    page->settings().setAcceleratedCompositingEnabled(false);
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif
    page->settings().setStandardFontFamily("serif"_s);
    page->settings().setSerifFontFamily("serif"_s);
    page->settings().setSansSerifFontFamily("sans-serif"_s);
    page->settings().setFixedFontFamily("monospace"_s);
    page->settings().setDefaultFontSize(16);
    page->settings().setDefaultFixedFontSize(13);
    auto scriptCountBefore = scarlet_jsc_execution_entry_count();
    auto scriptPermissionAttemptsBefore = ScriptController::scriptExecutionCount();
    RefPtr frame = page->localMainFrame();
    require(!!frame, "local main frame missing");
    frame->setView(LocalFrameView::create(*frame, size));
    frame->init();
    RefPtr view = frame->view();
    require(!!view, "local frame view missing");
    view->setCanHaveScrollbars(false);
    view->setScrollbarModes(ScrollbarMode::AlwaysOff, ScrollbarMode::AlwaysOff, true, true);
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);

    RefPtr loader = frame->loader().activeDocumentLoader();
    require(!!loader, "active document loader missing");
    auto& writer = loader->writer();
    writer.setMIMEType("text/html"_s);
    require(snapshotResources ? writer.begin(URL { String::fromUTF8(options.documentURL.c_str()) }) : writer.begin(), "begin real HTML document writer");
    writer.insertDataSynchronously(html);
    writer.end();
    RefPtr document = frame->document();
    require(!!document, "parsed document missing");
    view = frame->view();
    require(!!view, "parsed document view missing");
    std::string snapshotSettlement;
    if (snapshotResources)
        snapshotSettlement = settleSnapshot(*document, *view, options);
    else
        document->updateLayoutIgnorePendingStylesheets();
    require(view->renderView(), "WebCore render tree missing");
    require(!view->needsLayout(), "WebCore layout is incomplete");

    std::string elements = "[";
    unsigned elementIndex = 0;
    for (auto& id : options.inspectIDs) {
        auto element = document->getElementById(String::fromUTF8(id.c_str()));
        if (elementIndex++)
            elements += ',';
        elements += "{\"id\":" + jsonString(id) + ",\"found\":" + (element ? "true" : "false");
        if (element) {
            auto bounds = element->boundingClientRect();
            auto text = utf8(element->textContent());
            elements += ",\"bounds\":" + rectJSON(bounds) + ",\"text\":" + jsonString(text);
            require(printf("SCARLET_LAYOUT id=%s rect=%s,%s,%s,%s text_bytes=%zu\n", id.c_str(),
                number(bounds.x()).c_str(), number(bounds.y()).c_str(), number(bounds.width()).c_str(), number(bounds.height()).c_str(), text.size()) >= 0,
                "write layout log");
        }
        elements += '}';
    }
    elements += ']';
    auto selectors = selectorsJSON(*document, options);
    auto bodyText = document->body() ? utf8(document->body()->textContent()) : std::string();
    auto renderTree = utf8(externalRepresentation(frame.get()));
    require(!renderTree.empty(), "render tree text is empty");
    require(renderTree.size() <= maxOutputBytes, "render tree exceeds output limit");
    require(!view->needsLayout(), "layout invalidated before paint");
    require(view->layoutContext().inPaintableState(), "WebCore layout context cannot paint");

    auto surface = adoptRef(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size.width(), size.height()));
    require(cairo_surface_status(surface.get()) == CAIRO_STATUS_SUCCESS, "allocate Cairo surface");
    {
        GraphicsContextCairo context(surface.get());
        // Only the opaque page base is filled here. Every document pixel is
        // produced by WebCore's real render-layer tree in paintContents below.
        context.fillRect(display, Color::white);
        view->paintContents(context, IntRect { IntPoint { }, size });
        require(cairo_status(context.cr()) == CAIRO_STATUS_SUCCESS, "WebCore Cairo painting failed");
    }
    cairo_surface_flush(surface.get());
    require(cairo_surface_status(surface.get()) == CAIRO_STATUS_SUCCESS, "flush Cairo surface");
    auto* pixels = cairo_image_surface_get_data(surface.get());
    int stride = cairo_image_surface_get_stride(surface.get());
    require(pixels && stride >= size.width() * 4, "invalid Cairo pixel buffer");
    auto pixelBytes = static_cast<size_t>(stride) * options.height;
    require(pixelBytes <= maxOutputBytes, "pixel buffer exceeds output limit");
    std::span<const unsigned char> frameBytes { pixels, pixelBytes };
    auto pixelHash = hexHash(frameBytes);

    writeFile(temporaryOutput(0), frameBytes);
    require(cairo_surface_write_to_png(surface.get(), temporaryOutput(1).c_str()) == CAIRO_STATUS_SUCCESS, "write Cairo PNG");
    auto png = readFile(temporaryOutput(1), maxOutputBytes);
    writeFile(temporaryOutput(2), bytesOf(renderTree));
    std::string snapshotEvidence;
    if (snapshotResources) {
        auto state = scarletSnapshotResourceState();
        require(!state.pending && !state.failed && !state.blocked && !state.cancelled, "snapshot resources changed or failed during paint");
        require(!document->styleScope().hasPendingSheets() && !document->cachedResourceLoader().requestCount(), "snapshot loading changed during paint");
        snapshotEvidence = scarletSnapshotResourceEvidenceJSON();
    }

    bool scriptEnabled = page->settings().isScriptEnabled();
    bool imagesEnabled = page->settings().areImagesEnabled();
    bool loadsImagesAutomatically = page->settings().loadsImagesAutomatically();
    require(!snapshotResources || (imagesEnabled && loadsImagesAutomatically), "snapshot image settings were disabled");
    // Release the real Page graph before committing success. Page destruction
    // disconnects its view and detaches its frames; the remaining frame release
    // runs LocalFrame's document/loader cleanup. No exit/destructor is bypassed.
    teardownCheckpoint(options, "stop-loaders-begin");
    frame->loader().stopAllLoaders();
    teardownCheckpoint(options, "stop-loaders-end");
    surface = nullptr;
    teardownCheckpoint(options, "surface-released");
    document = nullptr;
    loader = nullptr;
    view = nullptr;
    teardownCheckpoint(options, "borrowed-references-released");
    page = nullptr;
    teardownCheckpoint(options, "page-released");
    frame = nullptr;
    teardownCheckpoint(options, "frame-released");
    // Font/Timer destruction must run while this thread's WebCore data is
    // still reachable. Otherwise native TLS clears m_clientData first, and
    // FontCache cleanup can create a second ThreadTimers while the old main
    // shared-timer callback remains registered. Use WebCore's real shutdown
    // API, then unregister/stop that callback; normal CRT/TLS cleanup follows.
    teardownCheckpoint(options, "thread-data-destroy-begin");
    auto& threadData = threadGlobalDataSingleton();
    threadData.destroy();
    teardownCheckpoint(options, "thread-data-destroy-end");
    threadData.threadTimers().setSharedTimer(nullptr);
    teardownCheckpoint(options, "shared-timer-detached");

    auto scriptCountAfter = scarlet_jsc_execution_entry_count();
    auto scriptPermissionAttemptsAfter = ScriptController::scriptExecutionCount();
    require(scriptCountAfter >= scriptCountBefore && scriptPermissionAttemptsAfter >= scriptPermissionAttemptsBefore, "script counter moved backwards");
    require(scriptCountBefore == scriptCountAfter, "JavaScript bytecode unexpectedly entered");
    require(!scriptEnabled, "script setting unexpectedly enabled");
    auto scriptExecutionEntries = scriptCountAfter - scriptCountBefore;
    auto scriptPermissionAttempts = scriptPermissionAttemptsAfter - scriptPermissionAttemptsBefore;
    require(printf("SCARLET_SCRIPT_GUARD run_id=%s execution_entries=%" PRIu64 " permission_attempts=%" PRIu64 " enabled=false\n",
        options.runID.c_str(), scriptExecutionEntries, scriptPermissionAttempts) >= 0, "write script guard log");

    std::string metadata = "{\n  \"schema\":2,\n  \"engine\":\"WebCore\",\n  \"port\":\"Scarlet\",\n  \"mode\":" + jsonString(snapshotResources ? "snapshot-resources-cpu" : "offline-direct-cpu")
        + ",\n  \"completion\":\"native-render-success\",\n  \"run_id\":" + jsonString(options.runID)
        + ",\n  \"output_prefix\":" + jsonString(options.prefix) + ",\n  \"revision\":" + jsonString(sourceRevision)
        + ",\n  \"input\":" + jsonString(options.input) + ",\n  \"input_bytes\":" + std::to_string(input.size())
        + ",\n  \"input_fnv1a64\":" + jsonString(hexHash(bytesOf(input)))
        + ",\n  \"viewport\":{\"width\":" + std::to_string(options.width) + ",\"height\":" + std::to_string(options.height) + "}"
        + ",\n  \"stride\":" + std::to_string(stride) + ",\n  \"pixel_bytes\":" + std::to_string(pixelBytes)
        + ",\n  \"pixel_format\":\"BGRA8-premultiplied-little-endian\",\n  \"pixel_fnv1a64\":" + jsonString(pixelHash)
        + ",\n  \"png_bytes\":" + std::to_string(png.size()) + ",\n  \"png_fnv1a64\":" + jsonString(hexHash(bytesOf(png)))
        + ",\n  \"render_tree_bytes\":" + std::to_string(renderTree.size()) + ",\n  \"render_tree_fnv1a64\":" + jsonString(hexHash(bytesOf(renderTree)))
        + ",\n  \"render_tree_present\":true,\n  \"layout_complete\":true,\n  \"scripts_executed\":" + std::to_string(scriptCountAfter - scriptCountBefore)
        + ",\n  \"script_execution_entries\":" + std::to_string(scriptExecutionEntries)
        + ",\n  \"script_permission_attempts\":" + std::to_string(scriptPermissionAttempts) + ",\n  \"script_enabled\":false"
        + ",\n  \"subresources_enabled\":" + (snapshotResources ? "true" : "false")
        + ",\n  \"virtual_display_equals_viewport\":true,\n  \"font_matches\":" + fonts
        + ",\n  \"body_text\":" + jsonString(bodyText) + ",\n  \"elements\":" + elements
        + ",\n  \"selectors\":" + selectors
        + (snapshotResources ? ",\n  \"resource_profile\":\"snapshotresources\",\n  \"document_url\":" + jsonString(options.documentURL)
            + ",\n  \"images_enabled\":" + (imagesEnabled ? "true" : "false")
            + ",\n  \"loads_images_automatically\":" + (loadsImagesAutomatically ? "true" : "false")
            + ",\n  \"snapshot_resource_root\":" + jsonString(options.snapshotResourceRoot) + ",\n  \"snapshot_index\":" + jsonString(options.snapshotIndex)
            + ",\n  \"resource_settlement\":" + snapshotSettlement + ",\n  \"resources\":" + snapshotEvidence : "")
        + ",\n  \"files\":{\"bgra\":" + jsonString(options.prefix + ".bgra") + ",\"png\":" + jsonString(options.prefix + ".png")
        + ",\"render_tree\":" + jsonString(options.prefix + ".render-tree.txt") + "}\n}\n";
    require(metadata.size() <= maxMetadataBytes, "metadata exceeds 8 MiB output limit");
    writeFile(temporaryOutput(3), bytesOf(metadata));

    publishOutput(0, options.prefix + ".bgra");
    publishOutput(1, options.prefix + ".png");
    publishOutput(2, options.prefix + ".render-tree.txt");
    publishOutput(3, options.prefix + ".json");
    printf("SCARLET_WEBCORE_PASS run_id=%s viewport=%ux%u stride=%d bytes=%zu pixel_fnv1a64=%s metadata_fnv1a64=%s scripts=0 output=%s\n",
        options.runID.c_str(), options.width, options.height, stride, pixelBytes, pixelHash.c_str(), hexHash(bytesOf(metadata)).c_str(), options.prefix.c_str());
    teardownCheckpoint(options, "main-return");
    return 0;
}
