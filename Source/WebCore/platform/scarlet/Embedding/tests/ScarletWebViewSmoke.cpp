/* Native C-ABI integration test. Fixture bytes are transport inputs; every DOM,
 * history, input and framebuffer observation comes from the real WebView. */
#include "../ScarletWebView.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <time.h>

namespace {

SWVBytes bytes(const char* value)
{
    return { reinterpret_cast<const uint8_t*>(value), std::strlen(value) };
}

SWVBytes bytes(const std::string& value)
{
    return { reinterpret_cast<const uint8_t*>(value.data()), value.size() };
}

std::string copy(SWVBytes value)
{
    return value.length ? std::string(reinterpret_cast<const char*>(value.data), value.length) : std::string { };
}

bool headerNameIs(SWVBytes value, const char* expected)
{
    if (value.length != std::strlen(expected) || (value.length && !value.data))
        return false;
    for (size_t index = 0; index < value.length; ++index) {
        unsigned char left = value.data[index];
        unsigned char right = static_cast<unsigned char>(expected[index]);
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right)
            return false;
    }
    return true;
}

void quoted(const std::string& value)
{
    std::putchar('"');
    for (unsigned char character : value) {
        if (character == '"' || character == '\\') {
            std::putchar('\\');
            std::putchar(character);
        } else if (character < 0x20)
            std::printf("\\u%04x", static_cast<unsigned>(character));
        else
            std::putchar(character);
    }
    std::putchar('"');
}

uint64_t milliseconds()
{
    timespec value { };
    if (clock_gettime(CLOCK_MONOTONIC, &value))
        return 0;
    return static_cast<uint64_t>(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
}

void pauseMilliseconds(unsigned duration)
{
    timespec remaining { static_cast<time_t>(duration / 1000), static_cast<long>(duration % 1000) * 1000000 };
    while (nanosleep(&remaining, &remaining)) { }
}

constexpr char firstURL[] = "https://smoke.example/first";
constexpr char secondURL[] = "https://smoke.example/second";
// A committed string starts with a newline and contains non-BMP Unicode. The
// next commit is U+F700, a private-use character rather than an OS key encoding.
constexpr char multilineCommit[] = "\n\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xf0\x9f\x8d\x93\n\xc3\xa9t\xc3\xa9";
constexpr char privateUseCommit[] = "\xef\x9c\x80";
constexpr char firstHTML[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Scarlet persistent first</title>
<link rel="stylesheet" href="/smoke.css">
<link rel="stylesheet" href="https://other.smoke.example/corp.css">
<script defer src="/smoke.js"></script></head><body>
<div id="paint-proof"></div><button id="click-proof" type="button">click-pending</button>
<input id="edit-proof" aria-label="native editing"><div id="edit-status">edit-pending</div>
<textarea id="multiline-proof" aria-label="multiline Unicode editing"></textarea>
<div id="multiline-status">multiline-pending</div>
<div id="key-proof">key-pending</div><div id="resize-proof">resize-pending</div>
<div id="script-proof">script-pending</div><div id="fetch-proof">fetch-pending</div>
<div id="cors-ok-proof">cors-ok-pending</div><div id="cors-block-proof">cors-block-pending</div>
<div id="corp-proof">corp-pending</div><div id="wheel-proof">wheel-pending</div>
<img id="image-proof" src="/proof.svg" width="32" height="32">
<img id="corp-image" src="https://other.smoke.example/corp.svg" width="32" height="32"
 onload="document.getElementById('corp-proof').textContent='corp-unexpected-load'"
 onerror="document.getElementById('corp-proof').textContent='corp-image-blocked'">
<div id="tail">real scrollable document</div></body></html>)HTML";
constexpr char firstCSS[] = R"CSS(html,body{margin:0;background:white;color:black;font:16px sans-serif}
#paint-proof{position:absolute;left:0;top:0;width:64px;height:64px;background:rgb(18,52,86)}
#click-proof{position:absolute;left:80px;top:8px;width:120px;height:40px}
#edit-proof{position:absolute;left:80px;top:64px;width:160px;height:32px;box-sizing:border-box}
#multiline-proof{position:absolute;left:280px;top:64px;width:120px;height:48px;box-sizing:border-box;resize:none}
#image-proof{position:absolute;left:240px;top:8px}
#corp-image{position:absolute;left:280px;top:8px}
#edit-status{margin-top:112px}#tail{margin-top:1200px;height:80px}
)CSS";
constexpr char firstJS[] = R"JS('use strict';
const byId = id => document.getElementById(id);
byId('script-proof').textContent = 'script-ran';
byId('click-proof').addEventListener('click', () => {
    byId('click-proof').textContent = 'clicked';
    byId('paint-proof').style.backgroundColor = 'rgb(70,80,90)';
});
document.addEventListener('keydown', event => {
    if (event.key === 'K') {
        byId('key-proof').textContent = 'key-down:K';
        event.preventDefault();
    }
});
byId('edit-proof').addEventListener('input', () => {
    byId('edit-status').textContent = 'edit:' + byId('edit-proof').value;
});
byId('multiline-proof').addEventListener('input', () => {
    byId('multiline-status').textContent = 'textarea:' + byId('multiline-proof').value;
});
const resized = () => { byId('resize-proof').textContent = innerWidth < 500 ? 'narrow' : 'wide'; };
window.addEventListener('resize', resized); resized();
window.addEventListener('scroll', () => { byId('wheel-proof').textContent = 'scrolled:' + scrollY; });
fetch('/same-origin.txt').then(response => response.text()).then(text => {
    byId('fetch-proof').textContent = text;
}).catch(() => { byId('fetch-proof').textContent = 'same-origin-failed'; });
fetch('https://other.smoke.example/cors-ok.txt').then(response => response.text()).then(text => {
    byId('cors-ok-proof').textContent = text;
}).catch(() => { byId('cors-ok-proof').textContent = 'cors-allowed-failed'; });
fetch('https://other.smoke.example/cors-block.txt').then(response => response.text()).then(() => {
    byId('cors-block-proof').textContent = 'cors-unexpected-success';
}).catch(() => { byId('cors-block-proof').textContent = 'cors-blocked'; });
)JS";
constexpr char proofSVG[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"32\"><rect width=\"32\" height=\"32\" fill=\"#d02040\"/></svg>";
constexpr char secondHTML[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<title>Scarlet persistent second</title><style>html,body{margin:0;background:rgb(30,100,140)}</style>
</head><body><div id="page-proof">second-real-document</div>
<div id="second-script-proof">script-pending</div>
<script>document.getElementById('second-script-proof').textContent='second-script-ran';</script></body></html>)HTML";
constexpr char attachmentHTML[] = "<!doctype html><title>Forbidden attachment</title><div id=attachment-proof>attachment-committed</div><script>document.getElementById('attachment-proof').textContent='attachment-executed';</script>";
constexpr char infiniteHTML[] = "<!doctype html><title>Termination probe</title><div id=termination-proof>not-entered</div><script>for(;;){document.getElementById('termination-proof').textContent='loop-body-entered';}</script>";

struct Route {
    const char* url;
    const char* mime;
    const char* body;
    const char* extraName;
    const char* extraValue;
    uint32_t status;
};
constexpr Route routes[] = {
    { firstURL, "text/html; charset=utf-8", firstHTML, nullptr, nullptr, 200 },
    { "https://smoke.example/smoke.css", "text/css; charset=utf-8", firstCSS, nullptr, nullptr, 200 },
    { "https://smoke.example/smoke.js", "text/javascript; charset=utf-8", firstJS, nullptr, nullptr, 200 },
    { "https://smoke.example/proof.svg", "image/svg+xml", proofSVG, nullptr, nullptr, 200 },
    { "https://smoke.example/same-origin.txt", "text/plain", "same-origin-body", nullptr, nullptr, 200 },
    { "https://other.smoke.example/cors-ok.txt", "text/plain", "cors-allowed-body", "Access-Control-Allow-Origin", "https://smoke.example", 200 },
    { "https://other.smoke.example/cors-block.txt", "text/plain", "must-not-reach-script", nullptr, nullptr, 200 },
    { "https://other.smoke.example/corp.css", "text/css", "#paint-proof{background:rgb(240,0,240)!important}", "Cross-Origin-Resource-Policy", "same-origin", 200 },
    { "https://other.smoke.example/corp.svg", "image/svg+xml", proofSVG, "Cross-Origin-Resource-Policy", "same-origin", 200 },
    { secondURL, "text/html; charset=utf-8", secondHTML, nullptr, nullptr, 200 },
    { "https://smoke.example/redirect", "text/plain", "", "Location", "/second", 302 },
    { "https://smoke.example/attachment", "text/html", attachmentHTML, "Content-Disposition", "attachment; filename=smoke.html", 200 },
    { "https://smoke.example/transport-failure", "text/plain", "", nullptr, nullptr, 200 },
    { "https://smoke.example/infinite", "text/html", infiniteHTML, nullptr, nullptr, 200 },
};
constexpr size_t routeCount = sizeof(routes) / sizeof(routes[0]);

struct State {
    std::string url;
    std::string title;
    std::string error;
    uint64_t generation { 0 };
    uint64_t entries { 0 };
    uint32_t loading { 0 };
    uint32_t pending { 0 };
    int32_t scrollY { 0 };
    bool back { false };
    bool forward { false };
};

struct Shared {
    std::string runID;
    std::string fonts { "/etc/fonts/fonts.conf" };
    bool termination { false };
    std::atomic<unsigned> phase { 0 }; // 1: owner ready, 2: infinite script armed, 3: owner finished.
    std::atomic<bool> continueOwner { false };
    std::atomic<ScarletWebView*> view { nullptr };
    std::atomic<bool> controlPassed { true };
    std::atomic<int32_t> terminationResult { SWV_EMPTY };
    bool ownerPassed { false }; // Published by pthread_join.
};

class Smoke {
public:
    explicit Smoke(Shared& shared) : m_shared(shared) { }

    bool check(bool condition, const char* stage, const std::string& detail = { }, int32_t status = SWV_OK)
    {
        std::printf("SCARLET_WEBVIEW_SMOKE {\"event\":\"check\",\"run_id\":"); quoted(m_shared.runID);
        std::printf(",\"stage\":"); quoted(stage);
        std::printf(",\"pass\":%s,\"status\":%d,\"detail\":", condition ? "true" : "false", status);
        quoted(detail); std::printf("}\n"); std::fflush(stdout);
        if (!condition)
            m_failed = true;
        return condition;
    }

    bool status(int32_t value, const char* stage) { return check(value == SWV_OK, stage, { }, value); }

    bool readState(State& state)
    {
        SWVState value { };
        if (!status(scarlet_webview_state(m_view, &value), "state"))
            return false;
        state = { copy(value.url), copy(value.title), copy(value.last_error), value.frame_generation,
            value.script_execution_entries, value.loading, value.pending_requests, value.scroll_y,
            !!value.can_go_back, !!value.can_go_forward };
        return true;
    }

    bool textIs(const char* id, const char* expected)
    {
        SWVBytes value { };
        return scarlet_webview_text_content(m_view, bytes(id), &value) == SWV_OK && copy(value) == expected;
    }

    bool tick()
    {
        auto result = scarlet_webview_tick(m_view);
        return result == SWV_OK || result == SWV_EMPTY || check(false, "tick", { }, result);
    }

    bool complete(const SWVRequest& request, size_t route)
    {
        const auto& fixture = routes[route];
        const std::string requestedURL = copy(request.url); // poll spans must not escape the next ABI mutation.
        if (!check(request.request_id && copy(request.method) == "GET" && !request.body.length
                && request.credentials_mode <= 2 && request.header_count <= 4096
                && (!request.header_count || request.headers), "request-contract", requestedURL))
            return false;
        if (route == 5 || route == 6) {
            bool originPresent = false;
            for (size_t index = 0; index < request.header_count; ++index) {
                if (headerNameIs(request.headers[index].name, "Origin")
                    && copy(request.headers[index].value) == "https://smoke.example")
                    originPresent = true;
            }
            if (!check(originPresent && request.credentials_mode == 1, "webcore-cors-request-metadata", requestedURL))
                return false;
        }
        ++m_requests[route];
        SWVHeader headers[] = {
            { bytes("Content-Type"), bytes(fixture.mime) },
            { bytes("Cache-Control"), bytes("no-store") },
            { bytes(fixture.extraName ? fixture.extraName : "X-Smoke-Fixture"), bytes(fixture.extraValue ? fixture.extraValue : "native-input") },
        };
        SWVResponse response { request.request_id, fixture.status, bytes(requestedURL), bytes(fixture.status == 302 ? "Found" : "OK"),
            headers, sizeof(headers) / sizeof(headers[0]), bytes(fixture.body), 0, { } };
        if (route == 12) {
            response.transport_error = 1;
            response.error_message = bytes("smoke injected transport failure");
        }
        if (m_shared.termination && route == 13)
            m_shared.phase.store(2, std::memory_order_release);
        if (!status(scarlet_webview_complete_request(m_view, &response), "complete-http-hop"))
            return false;
        if (!m_duplicateChecked && !m_shared.termination) {
            auto result = scarlet_webview_complete_request(m_view, &response);
            if (!check(result == SWV_STALE_REQUEST, "duplicate-completion-rejected", requestedURL, result))
                return false;
            m_duplicateChecked = true;
        }
        return true;
    }

    bool serviceRequests()
    {
        for (unsigned count = 0; count < 64; ++count) {
            SWVRequest request { };
            auto result = scarlet_webview_poll_request(m_view, &request);
            if (result == SWV_EMPTY)
                return true;
            if (!status(result, "poll-request"))
                return false;
            if (request.is_cancel) {
                ++m_cancellations;
                continue;
            }
            auto url = copy(request.url);
            size_t route = 0;
            while (route < routeCount && url != routes[route].url)
                ++route;
            if (!check(route < routeCount, "fixture-route", url) || !complete(request, route))
                return false;
        }
        return check(false, "request-drain-limit");
    }

    template<typename Predicate> bool settle(const char* stage, Predicate predicate, bool allowError = false)
    {
        const auto start = milliseconds();
        if (!check(start != 0, "native-monotonic-clock"))
            return false;
        for (unsigned iteration = 0; iteration < 30000 && milliseconds() - start < 30000; ++iteration) {
            if (!serviceRequests() || !tick())
                return false;
            State state;
            // Keep the hot loop quiet; publish its final observation below.
            SWVState value { };
            auto result = scarlet_webview_state(m_view, &value);
            if (result != SWV_OK)
                return check(false, stage, "state failed while settling", result);
            state = { copy(value.url), copy(value.title), copy(value.last_error), value.frame_generation,
                value.script_execution_entries, value.loading, value.pending_requests, value.scroll_y,
                !!value.can_go_back, !!value.can_go_forward };
            if (!allowError && !state.error.empty())
                return check(false, stage, state.error);
            if (!state.loading && !state.pending && predicate(state)) {
                std::printf("SCARLET_WEBVIEW_SMOKE {\"event\":\"settled\",\"run_id\":"); quoted(m_shared.runID);
                std::printf(",\"stage\":"); quoted(stage);
                std::printf(",\"url\":"); quoted(state.url);
                std::printf(",\"generation\":%llu,\"script_entries\":%llu,\"elapsed_ms\":%llu}\n",
                    static_cast<unsigned long long>(state.generation), static_cast<unsigned long long>(state.entries),
                    static_cast<unsigned long long>(milliseconds() - start));
                return check(true, stage);
            }
            pauseMilliseconds(1);
        }
        State state;
        readState(state);
        return check(false, stage, "timeout; url=" + state.url + "; error=" + state.error);
    }

    bool firstReady(const State& state)
    {
        return state.url == firstURL && state.title == "Scarlet persistent first"
            && textIs("script-proof", "script-ran") && textIs("fetch-proof", "same-origin-body")
            && textIs("cors-ok-proof", "cors-allowed-body") && textIs("cors-block-proof", "cors-blocked")
            && textIs("corp-proof", "corp-image-blocked");
    }

    bool secondReady(const State& state)
    {
        return state.url == secondURL && state.title == "Scarlet persistent second"
            && textIs("page-proof", "second-real-document") && textIs("second-script-proof", "second-script-ran");
    }

    bool pixels(uint32_t width, uint32_t height, uint64_t& generation, uint64_t& hash)
    {
        SWVPixels value { };
        if (!status(scarlet_webview_pixels(m_view, &value), "pixels"))
            return false;
        if (!check(value.data && value.width == width && value.height == height && value.format == 1
                && value.stride >= width * 4 && value.length == static_cast<size_t>(value.stride) * height
                && value.generation, "native-framebuffer-contract"))
            return false;
        generation = value.generation;
        hash = 14695981039346656037ull;
        for (size_t index = 0; index < value.length; ++index) {
            hash ^= value.data[index];
            hash *= 1099511628211ull;
        }
        std::printf("SCARLET_WEBVIEW_SMOKE {\"event\":\"frame\",\"run_id\":"); quoted(m_shared.runID);
        std::printf(",\"width\":%u,\"height\":%u,\"generation\":%llu,\"fnv1a64\":\"%016llx\"}\n",
            width, height, static_cast<unsigned long long>(generation), static_cast<unsigned long long>(hash));
        return true;
    }

    bool pixelIs(unsigned x, unsigned y, unsigned red, unsigned green, unsigned blue, const char* stage)
    {
        SWVPixels value { };
        auto result = scarlet_webview_pixels(m_view, &value);
        size_t offset = static_cast<size_t>(y) * value.stride + static_cast<size_t>(x) * 4;
        bool match = result == SWV_OK && value.data && x < value.width && y < value.height && offset + 4 <= value.length
            && value.data[offset] == blue && value.data[offset + 1] == green
            && value.data[offset + 2] == red && value.data[offset + 3] == 255;
        std::string actual;
        if (result == SWV_OK && value.data && offset + 4 <= value.length) {
            char description[96];
            std::snprintf(description, sizeof(description), "bgra=%u,%u,%u,%u at=%u,%u", static_cast<unsigned>(value.data[offset]),
                static_cast<unsigned>(value.data[offset + 1]), static_cast<unsigned>(value.data[offset + 2]),
                static_cast<unsigned>(value.data[offset + 3]), x, y);
            actual = description;
        }
        return check(match, stage, actual, result);
    }

    bool click(double x, double y)
    {
        uint32_t handled { 0 };
        SWVMouseEvent event { SWV_MOUSE_MOVE, SWV_BUTTON_NONE, 0, 0, x, y, x, y };
        if (!status(scarlet_webview_mouse(m_view, &event, &handled), "mouse-move"))
            return false;
        event.kind = SWV_MOUSE_DOWN; event.button = SWV_BUTTON_LEFT; event.click_count = 1;
        if (!status(scarlet_webview_mouse(m_view, &event, &handled), "mouse-down"))
            return false;
        event.kind = SWV_MOUSE_UP;
        return status(scarlet_webview_mouse(m_view, &event, &handled), "mouse-up");
    }

    bool basic()
    {
        State initial;
        if (!readState(initial) || !status(scarlet_webview_navigate(m_view, bytes(firstURL)), "navigate-first")
            || !settle("first-real-load-and-security", [this](const State& state) { return firstReady(state); }))
            return false;
        State first;
        if (!readState(first) || !check(first.entries > initial.entries, "real-script-entry")
            || !check(m_requests[0] && m_requests[1] && m_requests[2] && m_requests[3] && m_requests[4]
                && m_requests[5] && m_requests[6] && m_requests[7] && m_requests[8], "all-real-resource-hops")
            || !pixelIs(16, 16, 18, 52, 86, "css-paint-and-corp-stylesheet-block")
            || !pixelIs(256, 24, 208, 32, 64, "actual-svg-pixels"))
            return false;
        uint64_t originalGeneration { 0 }, originalHash { 0 };
        if (!pixels(640, 480, originalGeneration, originalHash) || !click(120, 24)
            || !settle("native-mouse-click-dom", [this](const State&) { return textIs("click-proof", "clicked"); })
            || !pixelIs(16, 16, 70, 80, 90, "script-repaint-pixels"))
            return false;
        uint64_t changedGeneration { 0 }, changedHash { 0 };
        if (!pixels(640, 480, changedGeneration, changedHash)
            || !check(changedGeneration > originalGeneration && changedHash != originalHash, "input-caused-frame-change"))
            return false;
        uint32_t handled { 0 };
        SWVKeyEvent key { SWV_KEY_DOWN, 0, 75, 0, bytes("K"), bytes("K"), bytes("K"), bytes("KeyK"), bytes("U+004B") };
        if (!status(scarlet_webview_key(m_view, &key, &handled), "native-key-down")
            || !check(handled == 1, "keydown-prevent-default-handled")
            || !settle("native-key-event-dom", [this](const State&) { return textIs("key-proof", "key-down:K"); }))
            return false;
        key.kind = SWV_KEY_UP;
        if (!status(scarlet_webview_key(m_view, &key, &handled), "native-key-up-after-prevent-default") || !click(120, 80))
            return false;
        key = { SWV_KEY_DOWN, 0, 81, 0, bytes("q"), bytes("q"), bytes("q"), bytes("KeyQ"), bytes("U+0051") };
        if (!status(scarlet_webview_key(m_view, &key, &handled), "editing-raw-key-down"))
            return false;
        if (!handled) {
            key.kind = SWV_KEY_CHAR;
            if (!status(scarlet_webview_key(m_view, &key, &handled), "editing-char"))
                return false;
        }
        key.kind = SWV_KEY_UP;
        if (!status(scarlet_webview_key(m_view, &key, &handled), "editing-key-up"))
            return false;
        key = { SWV_KEY_CHAR, 0, 0, 0, bytes("\xc3\xa9"), bytes("\xc3\xa9"), bytes("\xc3\xa9"), bytes(""), bytes("U+00E9") };
        if (!status(scarlet_webview_key(m_view, &key, &handled), "editing-committed-unicode")
            || !settle("real-native-editing-dom", [this](const State&) { return textIs("edit-status", "edit:q\xc3\xa9"); })
            || !click(320, 80))
            return false;
        key = { SWV_KEY_CHAR, 0, 0, 0, bytes(multilineCommit), bytes(multilineCommit), bytes(""), bytes(""), bytes("") };
        const std::string multilineExpected = "textarea:" + std::string(multilineCommit);
        if (!status(scarlet_webview_key(m_view, &key, &handled), "textarea-committed-multiline-unicode")
            || !settle("real-textarea-multiline-input-event", [this, &multilineExpected](const State&) {
                return textIs("multiline-status", multilineExpected.c_str());
            }))
            return false;
        key = { SWV_KEY_CHAR, 0, 0, 0, bytes(privateUseCommit), bytes(privateUseCommit), bytes(privateUseCommit), bytes(""), bytes("U+F700") };
        const std::string privateUseExpected = multilineExpected + privateUseCommit;
        if (!status(scarlet_webview_key(m_view, &key, &handled), "textarea-committed-private-use-unicode")
            || !settle("real-textarea-private-use-input-event", [this, &privateUseExpected](const State&) {
                return textIs("multiline-status", privateUseExpected.c_str());
            })
            || !status(scarlet_webview_resize(m_view, 420, 320, 1), "resize-narrow")
            || !settle("resize-event-and-layout", [this](const State&) { return textIs("resize-proof", "narrow"); }))
            return false;
        uint64_t narrowGeneration { 0 }, narrowHash { 0 };
        if (!pixels(420, 320, narrowGeneration, narrowHash)
            || !check(narrowGeneration > changedGeneration, "resize-generation")
            || !status(scarlet_webview_resize(m_view, 840, 640, 2), "resize-device-scale-two")
            || !settle("physical-to-css-resize", [this](const State&) { return textIs("resize-proof", "narrow"); })
            || !pixelIs(32, 32, 70, 80, 90, "physical-css-pixel-scaling")
            || !pixelIs(512, 48, 208, 32, 64, "physical-svg-pixel-scaling"))
            return false;
        uint64_t scaledGeneration { 0 }, scaledHash { 0 };
        if (!pixels(840, 640, scaledGeneration, scaledHash)
            || !status(scarlet_webview_resize(m_view, 640, 480, 1), "restore-desktop-size")
            || !settle("restore-resize-event", [this](const State&) { return textIs("resize-proof", "wide"); })
            || !status(scarlet_webview_focus(m_view, 0), "focus-out-before-idle")
            || !settle("unfocused-idle-baseline", [](const State&) { return true; }))
            return false;
        uint64_t idleGeneration { 0 }, idleHash { 0 };
        if (!pixels(640, 480, idleGeneration, idleHash))
            return false;
        for (unsigned iteration = 0; iteration < 64; ++iteration) {
            if (!serviceRequests() || !tick())
                return false;
            pauseMilliseconds(1);
        }
        uint64_t afterIdleGeneration { 0 }, afterIdleHash { 0 };
        if (!pixels(640, 480, afterIdleGeneration, afterIdleHash)
            || !check(afterIdleGeneration == idleGeneration && afterIdleHash == idleHash, "idle-frame-stays-stable")
            || !status(scarlet_webview_focus(m_view, 1), "focus-in-after-idle"))
            return false;
        SWVWheelEvent wheel { 0, 320, 240, 0, -160, 320, 240 };
        if (!status(scarlet_webview_wheel(m_view, &wheel, &handled), "native-wheel")
            || !settle("native-scroll-state", [this](const State& state) {
                SWVBytes value { };
                return state.scrollY > 0 && scarlet_webview_text_content(m_view, bytes("wheel-proof"), &value) == SWV_OK
                    && copy(value).find("scrolled:") == 0;
            })
            || !status(scarlet_webview_focus(m_view, 0), "focus-out")
            || !status(scarlet_webview_focus(m_view, 1), "focus-in")
            || !status(scarlet_webview_navigate(m_view, bytes(secondURL)), "navigate-second")
            || !settle("second-real-document", [this](const State& state) { return secondReady(state); }))
            return false;
        State second;
        if (!readState(second) || !check(second.back, "real-history-can-back")
            || !status(scarlet_webview_back(m_view), "history-back")
            || !settle("history-back-first", [this](const State& state) { return firstReady(state) && state.forward; })
            || !status(scarlet_webview_forward(m_view), "history-forward")
            || !settle("history-forward-second", [this](const State& state) { return secondReady(state) && state.back; }))
            return false;
        unsigned secondLoads = m_requests[9];
        if (!status(scarlet_webview_reload(m_view), "reload")
            || !settle("reload-real-http", [this](const State& state) { return secondReady(state); })
            || !check(m_requests[9] > secondLoads, "reload-request-observed")
            || !status(scarlet_webview_navigate(m_view, bytes("https://smoke.example/redirect")), "navigate-redirect")
            || !settle("webcore-followed-http-hop", [this](const State& state) { return secondReady(state); })
            || !check(m_requests[10] && m_requests[9] > secondLoads + 1, "redirect-destination-polled")
            || !status(scarlet_webview_navigate(m_view, bytes("https://smoke.example/attachment")), "navigate-attachment")
            || !settle("attachment-policy-rejected", [this](const State& state) {
                SWVBytes value { };
                return !state.error.empty() && state.url == secondURL
                    && scarlet_webview_text_content(m_view, bytes("attachment-proof"), &value) == SWV_EMPTY;
            }, true)
            || !status(scarlet_webview_navigate(m_view, bytes("https://smoke.example/transport-failure")), "navigate-transport-failure")
            || !settle("transport-failure-propagated", [](const State& state) {
                return state.error.find("smoke injected transport failure") != std::string::npos;
            }, true))
            return false;
        State final;
        return readState(final) && check(final.entries > first.entries, "later-native-script-entries") && !m_failed;
    }

    bool termination()
    {
        State before;
        if (!readState(before) || !status(scarlet_webview_navigate(m_view, bytes("https://smoke.example/infinite")), "navigate-infinite-script"))
            return false;
        // complete_request may synchronously run the parser, or tick may run
        // it later. The controller interrupts the existing VM from main.
        const auto start = milliseconds();
        for (unsigned iteration = 0; iteration < 30000 && milliseconds() - start < 30000; ++iteration) {
            if (!serviceRequests() || !tick())
                return false;
            if (m_requests[13] && m_shared.terminationResult.load(std::memory_order_acquire) != SWV_EMPTY) {
                State after;
                return readState(after)
                    && check(m_shared.terminationResult.load() == SWV_OK, "cross-thread-termination-returned")
                    && check(after.entries > before.entries && textIs("termination-proof", "loop-body-entered"),
                        "infinite-script-entered-and-owner-resumed");
            }
            pauseMilliseconds(1);
        }
        return check(false, "infinite-script-termination-timeout");
    }

    bool run()
    {
        bool initialized = status(scarlet_webview_initialize(bytes(m_shared.fonts)), "initialize-native-fonts");
        bool succeeded = false;
        if (initialized) {
            SWVConfiguration configuration { SCARLET_WEB_VIEW_ABI_VERSION, 640, 480, 1, 1 };
            if (status(scarlet_webview_create(&configuration, &m_view), "create-persistent-view")) {
                m_shared.view.store(m_view, std::memory_order_release);
                m_shared.phase.store(1, std::memory_order_release);
                const auto start = milliseconds();
                while (!m_shared.continueOwner.load(std::memory_order_acquire) && milliseconds() - start < 30000)
                    pauseMilliseconds(1);
                if (check(m_shared.continueOwner.load(), "owner-controller-handshake"))
                    succeeded = m_shared.termination ? termination() : basic();
                // The fixture transport holds no asynchronous task/body after
                // each borrowed completion. stopAllLoaders handles real loads.
                bool destroyed = status(scarlet_webview_destroy(m_view), "destroy-on-owner");
                m_view = nullptr;
                m_shared.view.store(nullptr, std::memory_order_release);
                succeeded = destroyed && succeeded;
            }
            succeeded = status(scarlet_webview_shutdown(), "shutdown-on-owner") && succeeded;
        }
        std::printf("SCARLET_WEBVIEW_SMOKE {\"event\":\"transport-counts\",\"run_id\":"); quoted(m_shared.runID);
        std::printf(",\"requests\":[");
        for (size_t index = 0; index < routeCount; ++index)
            std::printf("%s%u", index ? "," : "", m_requests[index]);
        std::printf("],\"cancellations\":%u}\n", m_cancellations);
        return succeeded && !m_failed;
    }

private:
    Shared& m_shared;
    ScarletWebView* m_view { nullptr };
    unsigned m_requests[routeCount] { };
    unsigned m_cancellations { 0 };
    bool m_duplicateChecked { false };
    bool m_failed { false };
};

void* ownerMain(void* context)
{
    auto& shared = *static_cast<Shared*>(context);
    Smoke smoke(shared);
    shared.ownerPassed = smoke.run();
    shared.phase.store(3, std::memory_order_release);
    return nullptr;
}

bool validRunID(const std::string& value)
{
    if (value.empty() || value.length() > 96)
        return false;
    for (unsigned char character : value) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '-' || character == '_'))
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Shared shared;
    for (int index = 1; index < argc; ++index) {
        if (!std::strcmp(argv[index], "--run-id") && index + 1 < argc)
            shared.runID = argv[++index];
        else if (!std::strcmp(argv[index], "--fonts") && index + 1 < argc)
            shared.fonts = argv[++index];
        else if (!std::strcmp(argv[index], "--mode") && index + 1 < argc) {
            const char* mode = argv[++index];
            if (std::strcmp(mode, "basic") && std::strcmp(mode, "termination"))
                return 2;
            shared.termination = !std::strcmp(mode, "termination");
        } else {
            std::fprintf(stderr, "usage: scarlet-webview-smoke --run-id TOKEN [--fonts /etc/fonts/fonts.conf] [--mode basic|termination]\n");
            return 2;
        }
    }
    if (!validRunID(shared.runID) || shared.fonts.empty())
        return 2;
    std::printf("SCARLET_WEBVIEW_SMOKE {\"event\":\"begin\",\"run_id\":"); quoted(shared.runID);
    std::printf(",\"mode\":\"%s\",\"abi_version\":%u,\"owner_stack_bytes\":8388608}\n",
        shared.termination ? "termination" : "basic", SCARLET_WEB_VIEW_ABI_VERSION);
    std::fflush(stdout);
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    if (result)
        return 1;
    result = pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
    pthread_t owner;
    if (!result)
        result = pthread_create(&owner, &attributes, ownerMain, &shared);
    pthread_attr_destroy(&attributes);
    if (result) {
        std::fprintf(stderr, "SCARLET_WEBVIEW_SMOKE pthread_create_status=%d\n", result);
        return 1;
    }
    const auto start = milliseconds();
    while (!shared.phase.load(std::memory_order_acquire) && milliseconds() - start < 30000)
        pauseMilliseconds(1);
    if (shared.phase.load(std::memory_order_acquire) == 1) {
        auto* view = shared.view.load(std::memory_order_acquire);
        SWVState state { };
        int32_t initializeResult = scarlet_webview_initialize(bytes(shared.fonts));
        int32_t stateResult = scarlet_webview_state(view, &state);
        bool confined = initializeResult == SWV_WRONG_THREAD && stateResult == SWV_WRONG_THREAD;
        shared.controlPassed.store(confined, std::memory_order_release);
        std::printf("SCARLET_WEBVIEW_SMOKE {\"event\":\"thread-confinement\",\"run_id\":"); quoted(shared.runID);
        std::printf(",\"pass\":%s,\"initialize_status\":%d,\"state_status\":%d}\n", confined ? "true" : "false", initializeResult, stateResult);
        shared.continueOwner.store(true, std::memory_order_release);
    } else
        shared.controlPassed.store(false, std::memory_order_release);
    if (shared.termination) {
        while (shared.phase.load(std::memory_order_acquire) != 2 && shared.phase.load(std::memory_order_acquire) != 3)
            pauseMilliseconds(1);
        if (shared.phase.load(std::memory_order_acquire) == 2) {
            pauseMilliseconds(250);
            auto terminationResult = scarlet_webview_request_termination();
            shared.terminationResult.store(terminationResult, std::memory_order_release);
            if (terminationResult != SWV_OK)
                shared.controlPassed.store(false, std::memory_order_release);
        } else
            shared.controlPassed.store(false, std::memory_order_release);
    }
    // No detached owner, bounded-join success, quick_exit, _Exit or TLS bypass.
    // An uninterruptible engine bug must be an outer guest timeout/failure.
    result = pthread_join(owner, nullptr);
    bool passed = !result && shared.ownerPassed && shared.controlPassed.load(std::memory_order_acquire);
    std::printf("SCARLET_WEBVIEW_SMOKE {\"event\":\"result\",\"run_id\":"); quoted(shared.runID);
    std::printf(",\"mode\":\"%s\",\"pass\":%s,\"owner_join_status\":%d,\"termination_status\":%d}\n",
        shared.termination ? "termination" : "basic", passed ? "true" : "false", result, shared.terminationResult.load());
    std::fflush(stdout);
    return passed ? 0 : 1;
}
