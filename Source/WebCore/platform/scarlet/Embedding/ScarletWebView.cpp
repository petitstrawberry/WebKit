// Native persistent WebCore Page embedding. Every call belongs to one owner
// worker. HTTP work is cloned by the host and completed back on that worker.
#include "config.h"
#include "ScarletWebView.h"
#include "ScarletEditorClient.h"
#include "ScarletChromeClient.h"
#if USE(COORDINATED_GRAPHICS)
#include "ScarletLayerTree.h"
#endif
#include "ScarletBackForwardClient.h"
#include "ScarletFrameLoaderClient.h"
#include "ScarletStorage.h"
#include "DatabaseProvider.h"
#include <JavaScriptCore/JSLock.h>
#include "CookieJar.h"
#include "StorageNamespaceProvider.h"
#include "ScarletLiveLoader.h"
#include "Color.h"
#include "BackForwardController.h"
#include "CommonVM.h"
#include "Document.h"
#include "DocumentView.h"
#include "Element.h"
#if ENABLE(VIDEO)
#include "DOMRect.h"
#include "HTMLCollection.h"
#include "HTMLVideoElement.h"
#include "MediaPlayer.h"
#include "MediaError.h"
#include "NodeDocument.h"
#include "TimeRanges.h"
#include "HTMLNames.h"
#include <wtf/JSONValues.h>
#endif
#include "EmptyClients.h"
#include "EventHandler.h"
#include "FocusController.h"
#include "FrameLoadRequest.h"
#include "FrameLoader.h"
#if USE(SKIA)
#include "GraphicsContextSkia.h"
#include "PlatformDisplay.h"
#include "ScarletSkiaVulkan.h"
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkColorSpace.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#else
#include "GraphicsContextCairo.h"
#endif
#include "HandleUserInputEventResult.h"
#include "HitTestRequest.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "LocalFrameView.h"
#include "Page.h"
#include "PageIdentifier.h"
#include "PageConfiguration.h"
#include "PlatformKeyboardEvent.h"
#include "PlatformMouseEvent.h"
#include "PlatformWheelEvent.h"
#include "PointerCaptureController.h"
#include "ProcessWarming.h"
#if USE(CAIRO)
#include "RefPtrCairo.h"
#endif
#include "ResourceRequest.h"
#include "SandboxFlags.h"
#include "ScarletPlatform.h"
#include "ScrollbarMode.h"
#include "ScrollingCoordinatorTypes.h"
#include "ScriptController.h"
#include "Settings.h"
#include "ThreadGlobalData.h"
#include "ThreadTimers.h"
#include <pal/SessionID.h>
#include <JavaScriptCore/VM.h>
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/URL.h>
#include <wtf/text/CString.h>
#if USE(CAIRO)
#include <cairo.h>
#endif
#include <array>
#include <fontconfig/fontconfig.h>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <span>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <vector>

#if !PLATFORM(SCARLET) || !OS(SCARLET) || (!USE(CAIRO) && !USE(SKIA))
#error "ScarletWebView requires native Scarlet graphics"
#endif
#if ENABLE(JIT) && ENABLE(C_LOOP)
#error "ScarletWebView JIT requires the native assembly LLInt"
#endif
// LowLevelInterpreter.cpp emits the generated assembly interpreter when C_LOOP
// is disabled. Scarlet currently supports that interpreter on ARM64 ELF only.
#if !ENABLE(C_LOOP) && !(CPU(ARM64) && CPU(ADDRESS64) && defined(__ELF__))
#error "ScarletWebView assembly LLInt requires 64-bit ARM64 ELF"
#endif
extern "C" uint64_t scarlet_jsc_execution_entry_count();

using namespace WebCore;
using namespace WTF::StringLiterals;

struct ScarletWebView {
    ScarletFrameLoadState loadState;
#if USE(COORDINATED_GRAPHICS)
    // ChromeClient borrows this tree. It must outlive Page and its GraphicsLayers,
    // including failure unwinding before the explicit destroy ABI is reached.
    std::unique_ptr<ScarletLayerTree> layerTree;
#endif
    RefPtr<Page> page;
    RefPtr<LocalFrame> frame;
#if USE(SKIA)
    // Accessed exclusively by the WebCore owner thread, like the page itself.
    struct PaintProfile {
        bool enabled { [] {
            const char* value = std::getenv("SCARLET_WEBVIEW_PROFILE");
            return value && !std::strcmp(value, "1");
        }() };
        bool started { false };
        MonotonicTime periodStart;
        struct Counters {
            uint64_t attempts { 0 }, failedSubmits { 0 }, full { 0 };
            uint64_t layout { 0 }, explicitFull { 0 }, dirtyFallback { 0 }, slotFull { 0 };
            uint64_t pixels { 0 }, paintLayoutMicros { 0 }, recordMicros { 0 }, submitMicros { 0 }, maxPaintMicros { 0 };
            uint64_t recordFlushMicros { 0 }, recordTreeMicros { 0 }, recordSceneMicros { 0 };
            uint64_t ticks { 0 }, tickMicros { 0 }, maxTickMicros { 0 };
            uint64_t runLoopMicros { 0 }, renderingUpdateMicros { 0 }, renderingFinalizeMicros { 0 }, sceneFlushMicros { 0 };
            uint64_t requests { 0 }, requestMicros { 0 }, maxRequestMicros { 0 };
            uint64_t gpuCompletions { 0 }, gpuCompletionMicros { 0 }, maxGpuCompletionMicros { 0 };
        } counters;
    } paintProfile;
    struct GpuSlot {
        std::unique_ptr<ScarletSkiaVulkan::Surface> surface;
        uint64_t lease { 0 };
        uint64_t imageIdentifier { 0 };
        bool pendingFullPaint { true };
        IntRect pendingDamage;
#if USE(COORDINATED_GRAPHICS)
        // Device pixels: nullopt means unknown contents, empty means current.
        // Leased allocations keep accumulating scene damage without being drawn.
        std::optional<Damage> pendingSceneDamage;
#endif
        uint32_t width { 0 }, height { 0 };
    };
    std::array<GpuSlot, 2> gpuSlots;
#if USE(COORDINATED_GRAPHICS)
    // At most one unpresented submission. A pending target is neither drawable
    // nor externally leased; the other allocation may still be held by the UI.
    struct PendingGpuFrame {
        GpuSlot* target { nullptr };
        std::unique_ptr<ScarletSkiaVulkan::Submission> submission;
        uint64_t paintedPixels { 0 };
        uint64_t generation { 0 };
        MonotonicTime submittedAt;
        bool ready { false };
    };
    std::optional<PendingGpuFrame> pendingGpuFrame;
#endif
    uint64_t nextImageIdentifier { 0 };
    ScarletSkiaVulkan::Surface* surface { nullptr };
#else
    RefPtr<cairo_surface_t> surface;
#endif
    uint32_t width { 0 };
    uint32_t height { 0 };
    double scale { 1 };
    uint64_t generation { 0 };
    unsigned short pressedButtons { 0 };
    DoublePoint previousMousePosition;
    bool hasMousePosition { false };
    std::string url;
    std::string title;
    std::string error;
    std::string inspectedText;
    SWVPaintStatistics paintStatistics { };
#if ENABLE(VIDEO)
    // Diagnostic snapshots, accessed only by the same owner as media_statistics.
    struct MediaProfile {
        bool enabled { [] {
            const char* value = std::getenv("SCARLET_WEBVIEW_PROFILE");
            return value && !std::strcmp(value, "1");
        }() };
        bool reported { false };
        uint32_t index { 0 };
        MonotonicTime lastReport;
        String currentSrc;
        String src;
        unsigned errorCode { 0 };
        String errorMessage;
    } mediaProfile;
#endif
    std::vector<uint8_t> publishedPixels;
};

namespace {
void traceTeardown(const char* stage)
{
    if (std::getenv("SCARLET_GC_DIAGNOSTICS"))
        std::fprintf(stderr, "SCARLET_WEBVIEW_TEARDOWN stage=%s\n", stage);
}

pthread_mutex_t initializationMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t owner;
std::atomic<bool> initialized { false };
std::atomic<bool> shutDown { false };
JSC::VM* terminationVM { nullptr }; // Published/revoked under initializationMutex.
std::unordered_set<ScarletWebView*> views;

bool ensureFontCacheDirectory(const std::string& path)
{
    if (path.empty())
        return false;
    // Create only missing components. Existing directories (including their
    // permissions) and non-directory paths are never changed.
    for (size_t end = 1; end <= path.size(); ++end) {
        if (end != path.size() && path[end] != '/')
            continue;
        std::string component = path.substr(0, end);
        struct stat info;
        if (!stat(component.c_str(), &info)) {
            if (!S_ISDIR(info.st_mode))
                return false;
            continue;
        }
        if (errno != ENOENT)
            return false;
        if (mkdir(component.c_str(), 0755) && errno != EEXIST)
            return false;
        if (stat(component.c_str(), &info) || !S_ISDIR(info.st_mode))
            return false;
    }
    return true;
}

void prepareFontCacheDirectories(FcConfig* fonts)
{
    FcStrList* directories = FcConfigGetCacheDirs(fonts);
    if (!directories)
        return;
    while (const FcChar8* directory = FcStrListNext(directories))
        ensureFontCacheDirectory(reinterpret_cast<const char*>(directory));
    FcStrListDone(directories);
}

bool validBytes(SWVBytes bytes, size_t limit)
{
    return bytes.length <= limit && (!bytes.length || bytes.data);
}
String fromBytes(SWVBytes bytes)
{
    return String::fromUTF8(std::span<const char>(reinterpret_cast<const char*>(bytes.data), bytes.length));
}
std::string toUTF8(const String& value)
{
    auto bytes = value.utf8();
    return { reinterpret_cast<const char*>(bytes.data()), bytes.length() };
}
SWVBytes borrow(const std::string& value)
{
    return { reinterpret_cast<const uint8_t*>(value.data()), value.size() };
}
int32_t ownerStatus()
{
    if (!initialized.load(std::memory_order_acquire) || shutDown.load(std::memory_order_acquire))
        return SWV_NOT_INITIALIZED;
    return pthread_equal(owner, pthread_self()) ? SWV_OK : SWV_WRONG_THREAD;
}
int32_t viewStatus(ScarletWebView* view)
{
    auto status = ownerStatus();
    if (status != SWV_OK)
        return status;
    return view && views.contains(view) ? SWV_OK : SWV_INVALID_ARGUMENT;
}
bool validSize(uint32_t width, uint32_t height, double scale)
{
    return width && height && width <= 4096 && height <= 4096
        && std::isfinite(scale) && scale >= .25 && scale <= 8
        && std::ceil(width / scale) <= 16384 && std::ceil(height / scale) <= 16384;
}
IntSize cssSize(const ScarletWebView& view)
{
    return { static_cast<int>(std::ceil(view.width / view.scale)), static_cast<int>(std::ceil(view.height / view.scale)) };
}
OptionSet<PlatformEvent::Modifier> modifiers(uint32_t flags)
{
    OptionSet<PlatformEvent::Modifier> result;
    if (flags & SWV_SHIFT) result.add(PlatformEvent::Modifier::ShiftKey);
    if (flags & SWV_CONTROL) result.add(PlatformEvent::Modifier::ControlKey);
    if (flags & SWV_ALT) result.add(PlatformEvent::Modifier::AltKey);
    if (flags & SWV_META) result.add(PlatformEvent::Modifier::MetaKey);
    if (flags & SWV_CAPS_LOCK) result.add(PlatformEvent::Modifier::CapsLockKey);
    return result;
}
bool validModifiers(uint32_t value) { return !(value & ~31u); }
void configureView(LocalFrameView& view)
{
    configureScarletFrameView(view);
}
void screenForView(ScarletWebView& view)
{
    auto size = cssSize(view);
    FloatRect rect { 0, 0, static_cast<float>(size.width()), static_cast<float>(size.height()) };
    view.loadState.viewport = rect;
    setScarletScreenConfiguration(rect, rect);
}
#if ENABLE(VIDEO)
// These getters inspect the existing DOM/player only. In particular, the 1Hz
// work profile must not measure itself by triggering layout for client rects.
RefPtr<HTMLVideoElement> mediaElementAt(ScarletWebView& view, uint32_t index, uint32_t& videoCount)
{
    videoCount = 0;
    RefPtr document = view.frame ? view.frame->document() : nullptr;
    if (!document)
        return nullptr;
    auto elements = document->getElementsByTagName(AtomString { "video"_s });
    RefPtr<HTMLVideoElement> video;
    for (unsigned i = 0; i < elements->length(); ++i) {
        RefPtr candidate = dynamicDowncast<HTMLVideoElement>(elements->item(i));
        if (!candidate)
            continue;
        if (videoCount == index)
            video = candidate;
        ++videoCount;
    }
    return video;
}

void collectMediaCounters(HTMLVideoElement& video, ScarletWebViewMediaStatistics& out)
{
    out.width = video.videoWidth();
    out.height = video.videoHeight();
    out.paused = video.paused();
    out.ended = video.ended();
    out.ready_state = static_cast<uint32_t>(video.readyState());
    out.current_time = video.currentTime();
    out.duration = video.duration();
    if (RefPtr player = video.player()) {
        out.decoded_frames = player->decodedFrameCount();
        out.dropped_frames = player->droppedFrameCount();
        if (auto metrics = player->videoPlaybackQualityMetrics())
            out.presented_frames = metrics->displayCompositedVideoFrames;
    }
}
#endif

#if USE(SKIA)
uint64_t profileMicroseconds(MonotonicTime start, MonotonicTime end)
{
    double micros = (end - start).microseconds();
    if (!(micros > 0))
        return 0;
    if (micros >= static_cast<double>(UINT64_MAX))
        return UINT64_MAX;
    return static_cast<uint64_t>(micros);
}

void addProfileCounter(uint64_t& counter, uint64_t amount)
{
    counter = amount > UINT64_MAX - counter ? UINT64_MAX : counter + amount;
}

MonotonicTime beginProfile(ScarletWebView& view)
{
    if (!view.paintProfile.enabled)
        return { };
    auto start = MonotonicTime::now();
    if (!view.paintProfile.started) {
        view.paintProfile.started = true;
        view.paintProfile.periodStart = start;
    }
    return start;
}

void reportProfile(ScarletWebView& view, MonotonicTime end)
{
    auto& profile = view.paintProfile;
    // Begin before the work: even a single long first operation is reported.
    if (!profile.enabled || (end - profile.periodStart).seconds() < 1.0)
        return;
    ScarletWebViewMediaStatistics media { };
#if ENABLE(VIDEO)
    if (RefPtr video = mediaElementAt(view, 0, media.video_count))
        collectMediaCounters(*video, media);
#endif
    String mediaTime = "none"_s;
    if (media.video_count)
        mediaTime = String::number(media.current_time);
    auto mediaTimeBytes = mediaTime.utf8();
    int skiaBudgetedResources = 0;
    size_t skiaBudgetedBytes = 0, skiaPurgeableBytes = 0, skiaLimitBytes = 0;
    // Read existing owner-context CPU counters only: no lazy context creation,
    // layout, resource purge, work-completion polling, or GPU submission.
    if (auto* display = PlatformDisplay::sharedDisplayIfExists()) {
        if (auto* context = display->skiaGrContext()) {
            context->getResourceCacheUsage(&skiaBudgetedResources, &skiaBudgetedBytes);
            skiaPurgeableBytes = context->getResourceCachePurgeableBytes();
            skiaLimitBytes = context->getResourceCacheLimit();
        }
    }
    const auto& c = profile.counters;
    auto printable = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    std::fprintf(stderr, "SCARLET_WEBVIEW_PROFILE view=%p size=%ux%u period_us=%llu attempts=%llu failed_submits=%llu full=%llu layout=%llu explicit_full=%llu dirty_fallback=%llu slot_full=%llu painted_pixels=%llu paint_layout_us=%llu record_us=%llu record_flush_us=%llu record_tree_us=%llu record_scene_us=%llu submit_us=%llu max_paint_us=%llu ticks=%llu tick_us=%llu max_tick_us=%llu runloop_us=%llu rendering_update_us=%llu rendering_finalize_us=%llu scene_flush_us=%llu complete_requests=%llu complete_request_us=%llu max_complete_request_us=%llu gpu_completions=%llu gpu_completion_us=%llu max_gpu_completion_us=%llu skia_budgeted_resources=%d skia_budgeted_bytes=%llu skia_purgeable_bytes=%llu skia_limit_bytes=%llu video_index=0 video_count=%u video_width=%u video_height=%u video_decoded=%llu video_dropped=%llu video_presented=%llu video_time=%s\n",
        static_cast<void*>(&view), view.width, view.height, printable(profileMicroseconds(profile.periodStart, end)),
        printable(c.attempts), printable(c.failedSubmits), printable(c.full), printable(c.layout), printable(c.explicitFull),
        printable(c.dirtyFallback), printable(c.slotFull), printable(c.pixels), printable(c.paintLayoutMicros),
        printable(c.recordMicros), printable(c.recordFlushMicros), printable(c.recordTreeMicros), printable(c.recordSceneMicros),
        printable(c.submitMicros), printable(c.maxPaintMicros), printable(c.ticks),
        printable(c.tickMicros), printable(c.maxTickMicros), printable(c.runLoopMicros), printable(c.renderingUpdateMicros),
        printable(c.renderingFinalizeMicros), printable(c.sceneFlushMicros), printable(c.requests), printable(c.requestMicros), printable(c.maxRequestMicros),
        printable(c.gpuCompletions), printable(c.gpuCompletionMicros), printable(c.maxGpuCompletionMicros),
        skiaBudgetedResources, printable(skiaBudgetedBytes), printable(skiaPurgeableBytes), printable(skiaLimitBytes),
        media.video_count, media.width, media.height, printable(media.decoded_frames),
        printable(media.dropped_frames), printable(media.presented_frames), reinterpret_cast<const char*>(mediaTimeBytes.data()));
    profile.counters = { };
    profile.periodStart = end;
}

// tick includes the shared RunLoop cycle and Page rendering updates; request
// completion includes synchronous resource callbacks (such as script execution).
class ProfileOperation {
public:
    enum class Kind { Tick, CompleteRequest };
    ProfileOperation(ScarletWebView& view, Kind kind)
        : m_view(view), m_kind(kind), m_start(beginProfile(view)) { }
    ~ProfileOperation()
    {
        if (!m_view.paintProfile.enabled)
            return;
        auto end = MonotonicTime::now();
        auto elapsed = profileMicroseconds(m_start, end);
        auto& c = m_view.paintProfile.counters;
        auto& count = m_kind == Kind::Tick ? c.ticks : c.requests;
        auto& total = m_kind == Kind::Tick ? c.tickMicros : c.requestMicros;
        auto& maximum = m_kind == Kind::Tick ? c.maxTickMicros : c.maxRequestMicros;
        addProfileCounter(count, 1);
        addProfileCounter(total, elapsed);
        if (elapsed > maximum)
            maximum = elapsed;
        reportProfile(m_view, end);
    }
private:
    ScarletWebView& m_view;
    Kind m_kind;
    MonotonicTime m_start;
};

// Non-overlapping tick wall phases. The RunLoop phase includes script execution
// and synchronous host/resource callbacks; it is not a pure JavaScript timer.
class ProfileTickPhase {
public:
    ProfileTickPhase(ScarletWebView& view, uint64_t& counter)
        : m_counter(counter)
        , m_enabled(view.paintProfile.enabled)
        , m_start(m_enabled ? MonotonicTime::now() : MonotonicTime { })
    { }
    ~ProfileTickPhase()
    {
        if (m_enabled)
            addProfileCounter(m_counter, profileMicroseconds(m_start, MonotonicTime::now()));
    }
private:
    uint64_t& m_counter;
    bool m_enabled;
    MonotonicTime m_start;
};

void recordPaintProfile(ScarletWebView& view, const IntRect& damage, bool neededLayout, bool explicitFull,
    bool dirtyFallback, bool slotFull, bool submitted, MonotonicTime start, MonotonicTime layoutEnd,
    MonotonicTime recordStart, MonotonicTime recordEnd, MonotonicTime end,
    std::optional<uint64_t> physicalPixels = std::nullopt)
{
    auto& c = view.paintProfile.counters;
    addProfileCounter(c.attempts, 1);
    addProfileCounter(c.failedSubmits, !submitted);
    addProfileCounter(c.full, physicalPixels
        ? *physicalPixels == static_cast<uint64_t>(view.width) * view.height
        : damage == IntRect { IntPoint { }, cssSize(view) });
    addProfileCounter(c.layout, neededLayout);
    addProfileCounter(c.explicitFull, explicitFull);
    addProfileCounter(c.dirtyFallback, dirtyFallback);
    addProfileCounter(c.slotFull, slotFull);
    FloatRect scaledDamage { damage };
    scaledDamage.scale(view.scale);
    auto physicalDamage = enclosingIntRect(scaledDamage);
    physicalDamage.intersect(IntRect { 0, 0, static_cast<int>(view.width), static_cast<int>(view.height) });
    if (submitted)
        addProfileCounter(c.pixels, physicalPixels.value_or(static_cast<uint64_t>(physicalDamage.width()) * physicalDamage.height()));
    // Page::updateRendering may already have laid out during tick. This measures
    // only updateLayoutIgnorePendingStylesheets executed by this paint call.
    addProfileCounter(c.paintLayoutMicros, profileMicroseconds(start, layoutEnd));
    addProfileCounter(c.recordMicros, profileMicroseconds(recordStart, recordEnd));
    addProfileCounter(c.submitMicros, profileMicroseconds(recordEnd, end));
    auto elapsed = profileMicroseconds(start, end);
    if (elapsed > c.maxPaintMicros)
        c.maxPaintMicros = elapsed;
    reportProfile(view, end);
}
#endif

#if USE(COORDINATED_GRAPHICS)
void discardPendingGpuFrame(ScarletWebView& view)
{
    if (!view.pendingGpuFrame)
        return;
    auto* target = view.pendingGpuFrame->target;
    // Submission destruction drains only when it has not actually completed.
    // Do this before changing page/layer ownership or freeing output surfaces.
    view.pendingGpuFrame.reset();
    target->pendingSceneDamage.reset();
    view.surface = nullptr;
    view.layerTree->discardPendingPresentationCallbacks();
}

int32_t completePendingGpuFrame(ScarletWebView& view)
{
    if (!view.pendingGpuFrame || view.pendingGpuFrame->ready)
        return SWV_OK;
    auto& pending = *view.pendingGpuFrame;
    auto device = PlatformDisplay::sharedDisplay().skiaVulkanContext();
    auto status = device ? device->submissionStatus(*pending.submission) : ScarletSkiaVulkan::SubmissionStatus::Failed;
    if (status == ScarletSkiaVulkan::SubmissionStatus::Pending)
        return SWV_EMPTY;
    if (status == ScarletSkiaVulkan::SubmissionStatus::Failed) {
        discardPendingGpuFrame(view);
        view.loadState.needsPaint = true;
        view.loadState.needsRenderingUpdate = true;
        view.loadState.error = "WebCore Vulkan layer submission failed."_s;
        return SWV_FAILED;
    }

    auto completedAt = MonotonicTime::now();
    pending.submission.reset(); // Finished: no synchronous GPU wait.
    pending.ready = true;
    // Only this allocation caught up to the submitted scene. New scene changes
    // remain on their retained layers and fan out during the next paint.
    pending.target->pendingSceneDamage.emplace(IntSize { static_cast<int>(view.width), static_cast<int>(view.height) });
    ++view.paintStatistics.paints;
    view.paintStatistics.painted_pixels += pending.paintedPixels;
    pending.generation = ++view.generation;
    if (view.paintProfile.enabled) {
        auto elapsed = profileMicroseconds(pending.submittedAt, completedAt);
        auto& counters = view.paintProfile.counters;
        addProfileCounter(counters.gpuCompletions, 1);
        addProfileCounter(counters.gpuCompletionMicros, elapsed);
        counters.maxGpuCompletionMicros = std::max(counters.maxGpuCompletionMicros, elapsed);
        reportProfile(view, completedAt);
    }
    view.layerTree->didSubmit(completedAt);
    view.page->didCompleteRenderingFrame();
    return SWV_OK;
}

int32_t paintCoordinated(ScarletWebView& view)
{
    RefPtr frameView = view.frame->view();
    RefPtr document = view.frame->document();
    if (!frameView || !document)
        return SWV_EMPTY;
    if (!view.loadState.needsPaint && !frameView->needsLayout() && view.surface && !view.layerTree->needsComposition())
        return SWV_EMPTY;

    bool neededLayout = frameView->needsLayout();
    bool explicitFullPaint = view.loadState.fullPaint;
    auto profileStart = beginProfile(view);
    document->updateLayoutIgnorePendingStylesheets();
    auto profileLayoutEnd = view.paintProfile.enabled ? MonotonicTime::now() : MonotonicTime { };
    if (!frameView->renderView() || frameView->needsLayout() || !frameView->layoutContext().inPaintableState())
        return SWV_EMPTY;

    ScarletWebView::GpuSlot* target = nullptr;
    for (auto& slot : view.gpuSlots) {
        if (slot.surface.get() == view.surface)
            target = &slot;
    }
    if (!target || target->lease)
        return SWV_FAILED;

    // Clear only the DOM invalidations consumed by this flush. New callbacks
    // raised while rasterizing or presenting must remain pending for the next tick.
    view.loadState.needsPaint = false;
    view.loadState.fullPaint = false;
    view.loadState.dirtyRect = { };
    auto profileRecordStart = view.paintProfile.enabled ? MonotonicTime::now() : MonotonicTime { };
    // Paint may follow input or a resource callback before the next host tick.
    // Flush any layout/property changes through WebCore before committing them.
    bool compositingFlushed = frameView->flushCompositingStateIncludingSubframes();
    auto profileFlushEnd = view.paintProfile.enabled ? MonotonicTime::now() : MonotonicTime { };
    bool treeFlushed = compositingFlushed && view.layerTree->flush(*frameView);
    auto profileTreeEnd = view.paintProfile.enabled ? MonotonicTime::now() : MonotonicTime { };
    if (!treeFlushed) {
        view.loadState.needsPaint = true;
        view.loadState.needsRenderingUpdate = true;
        return SWV_EMPTY;
    }

    const IntSize deviceSize { static_cast<int>(view.width), static_cast<int>(view.height) };
    const auto priorTargetDamage = target->pendingSceneDamage;
    std::optional<Damage> sceneDamage { std::in_place, deviceSize };
    view.page->willStartRenderingUpdateDisplay();
    // The tree applies DPR on its scene root. A second canvas scale would
    // incorrectly scale both content and the device-space damage.
    view.layerTree->paint(*view.surface->skSurface().getCanvas(), sceneDamage, priorTargetDamage, SK_ColorWHITE);
    auto profileRecordEnd = view.paintProfile.enabled ? MonotonicTime::now() : MonotonicTime { };

    // The retained scene consumes its current damage during paint. Preserve
    // that delta for every output allocation before any submission can fail.
    for (auto& slot : view.gpuSlots) {
        if (!sceneDamage)
            slot.pendingSceneDamage.reset();
        else if (slot.pendingSceneDamage)
            slot.pendingSceneDamage->add(*sceneDamage);
    }

    if (priorTargetDamage && sceneDamage && priorTargetDamage->isEmpty() && sceneDamage->isEmpty()) {
        view.layerTree->discardPendingPresentationCallbacks();
        view.page->didCompleteRenderingUpdateDisplay();
        return SWV_EMPTY;
    }

    Damage repaintDamage { deviceSize, Damage::Mode::Full };
    if (priorTargetDamage && sceneDamage) {
        repaintDamage = *priorTargetDamage;
        repaintDamage.add(*sceneDamage);
    }
    // Match the compositor's actual non-overlapping paint rects rather than a
    // bounding box (distant small changes must not count as a full repaint).
    uint64_t paintedPixels = 0;
    const IntRect viewport { IntPoint { }, deviceSize };
    for (auto rect : repaintDamage.rectsForPainting()) {
        rect.intersect(viewport);
        paintedPixels += static_cast<uint64_t>(rect.width()) * rect.height();
    }
    auto device = PlatformDisplay::sharedDisplay().skiaVulkanContext();
    auto submittedAt = view.paintProfile.enabled ? MonotonicTime::now() : MonotonicTime { };
    auto submission = device ? device->submitAsync(*view.surface) : nullptr;
    bool submitted = submission && device->submissionStatus(*submission) != ScarletSkiaVulkan::SubmissionStatus::Failed;
    auto submitReturnedAt = view.paintProfile.enabled ? MonotonicTime::now() : MonotonicTime { };
    if (view.paintProfile.enabled) {
        // Partition record_us for the same paint attempts. The scene interval
        // includes display-update preparation immediately before scene.paint.
        auto& counters = view.paintProfile.counters;
        addProfileCounter(counters.recordFlushMicros, profileMicroseconds(profileRecordStart, profileFlushEnd));
        addProfileCounter(counters.recordTreeMicros, profileMicroseconds(profileFlushEnd, profileTreeEnd));
        addProfileCounter(counters.recordSceneMicros, profileMicroseconds(profileTreeEnd, profileRecordEnd));
        recordPaintProfile(view, IntRect { IntPoint { }, cssSize(view) }, neededLayout, explicitFullPaint,
            false, !priorTargetDamage, submitted, profileStart, profileLayoutEnd, profileRecordStart, profileRecordEnd,
            submitReturnedAt, paintedPixels);
    }
    view.page->didCompleteRenderingUpdateDisplay();
    if (!submitted) {
        // A failed target is no longer trusted. The other slot still owns the
        // scene delta above and can catch up when its lease is returned.
        target->pendingSceneDamage.reset();
        view.loadState.needsPaint = true;
        view.layerTree->discardPendingPresentationCallbacks();
        view.loadState.error = "WebCore Vulkan layer submission failed."_s;
        return SWV_FAILED;
    }

    // Even an immediately finished submission first enters this reserved state.
    // Publication, generation advancement and media feedback happen only after
    // the owner has observed successful GPU completion.
    view.pendingGpuFrame.emplace();
    view.pendingGpuFrame->target = target;
    view.pendingGpuFrame->submission = std::move(submission);
    view.pendingGpuFrame->paintedPixels = paintedPixels;
    view.pendingGpuFrame->submittedAt = submittedAt;
    return SWV_EMPTY;
}
#endif

int32_t paint(ScarletWebView& view)
{
#if USE(COORDINATED_GRAPHICS)
    return paintCoordinated(view);
#else
    RefPtr frameView = view.frame->view();
    RefPtr document = view.frame->document();
    if (!frameView || !document)
        return SWV_EMPTY;
    if (!view.loadState.needsPaint && !frameView->needsLayout() && view.surface)
        return SWV_OK;
    bool neededLayout = frameView->needsLayout();
#if USE(SKIA)
    auto profileStart = beginProfile(view);
    MonotonicTime profileLayoutEnd, profileRecordStart, profileRecordEnd;
#endif
    document->updateLayoutIgnorePendingStylesheets();
#if USE(SKIA)
    if (view.paintProfile.enabled)
        profileLayoutEnd = MonotonicTime::now();
#endif
    if (!frameView->renderView())
        return SWV_EMPTY;
    if (frameView->needsLayout() || !frameView->layoutContext().inPaintableState())
        return SWV_EMPTY;

    auto size = cssSize(view);
    IntRect damage { IntPoint { }, size };
#if USE(SKIA)
    ScarletWebView::GpuSlot* target = nullptr;
    for (auto& slot : view.gpuSlots) {
        if (slot.surface.get() == view.surface)
            target = &slot;
    }
    // An acquired page image is immutable until its owner lease is returned.
    if (!target || target->lease) return SWV_FAILED;
    // Layout may generate more damage. Fan it out only after layout and before
    // consuming the global invalidation, including slots leased by the UI.
    bool explicitFullPaint = view.loadState.fullPaint;
    bool dirtyFallback = view.loadState.needsPaint && view.loadState.dirtyRect.isEmpty();
    bool fullPaint = neededLayout || explicitFullPaint || dirtyFallback;
    for (auto& slot : view.gpuSlots) {
        if (fullPaint) {
            slot.pendingFullPaint = true;
            slot.pendingDamage = { };
        } else if (!slot.pendingFullPaint)
            slot.pendingDamage.unite(view.loadState.dirtyRect);
    }
    bool targetFullPaint = target->pendingFullPaint;
    if (!target->pendingFullPaint)
        damage.intersect(target->pendingDamage);
#else
    if (view.surface && !view.loadState.fullPaint && !view.loadState.dirtyRect.isEmpty())
        damage.intersect(view.loadState.dirtyRect);
#endif
    view.loadState.needsPaint = false;
    view.loadState.fullPaint = false;
    view.loadState.dirtyRect = { };
    if (damage.isEmpty()) {
#if USE(SKIA)
        target->pendingDamage = { };
        return SWV_EMPTY;
#else
        return SWV_OK;
#endif
    }
#if USE(SKIA)
    if (!view.surface) return SWV_FAILED;
    if (view.paintProfile.enabled)
        profileRecordStart = MonotonicTime::now();
    {
        GraphicsContextSkia context(*view.surface->skSurface().getCanvas(), RenderingMode::Accelerated, RenderingPurpose::DOM);
        context.save();
        context.scale(FloatSize { static_cast<float>(view.scale), static_cast<float>(view.scale) });
        context.clip(FloatRect { damage });
        context.fillRect(FloatRect { damage }, Color::white);
        frameView->paint(context, damage);
        context.restore();
    }
    if (view.paintProfile.enabled)
        profileRecordEnd = MonotonicTime::now();
    auto device = PlatformDisplay::sharedDisplay().skiaVulkanContext();
    bool submitted = device && device->submit(*view.surface);
    if (view.paintProfile.enabled)
        recordPaintProfile(view, damage, neededLayout, explicitFullPaint, dirtyFallback, targetFullPaint,
            submitted, profileStart, profileLayoutEnd, profileRecordStart, profileRecordEnd, MonotonicTime::now());
    if (!submitted) {
        view.loadState.error = "WebCore Vulkan page submission failed."_s;
        return SWV_FAILED;
    }
    // Only this allocation has caught up. Other slots retain all missed damage.
    target->pendingFullPaint = false;
    target->pendingDamage = { };
#else
    auto surface = view.surface;
    if (!surface)
        surface = adoptRef(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, view.width, view.height));
    if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
        view.loadState.error = "Unable to allocate Cairo WebView framebuffer."_s;
        return SWV_FAILED;
    }
    {
        GraphicsContextCairo context(surface.get());
        context.scale(FloatSize { static_cast<float>(view.scale), static_cast<float>(view.scale) });
        context.clip(FloatRect { damage });
        context.fillRect(FloatRect { damage }, Color::white);
        // ScrollView::paint applies the actual scroll offset and viewport clip;
        // paintContents alone would incorrectly redraw the document's top.
        frameView->paint(context, damage);
        if (cairo_status(context.cr()) != CAIRO_STATUS_SUCCESS) {
            view.loadState.error = "WebCore Cairo paint failed."_s;
            return SWV_FAILED;
        }
    }
    cairo_surface_flush(surface.get());
    if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS)
        return SWV_FAILED;
    view.surface = WTF::move(surface);
    auto stride = cairo_image_surface_get_stride(view.surface.get());
    auto* bytes = cairo_image_surface_get_data(view.surface.get());
    size_t length = static_cast<size_t>(stride) * view.height;
    bool changed = view.publishedPixels.size() != length;
    if (changed)
        view.publishedPixels.resize(length);
#endif
    FloatRect scaledDamage { damage };
    scaledDamage.scale(view.scale);
    auto deviceDamage = enclosingIntRect(scaledDamage);
    deviceDamage.intersect(IntRect { 0, 0, static_cast<int>(view.width), static_cast<int>(view.height) });
    ++view.paintStatistics.paints;
    view.paintStatistics.painted_pixels += static_cast<uint64_t>(deviceDamage.width()) * deviceDamage.height();
#if USE(SKIA)
    ++view.generation;
#else
    size_t rowLength = static_cast<size_t>(deviceDamage.width()) * 4;
    for (int row = deviceDamage.y(); row < deviceDamage.maxY(); ++row) {
        size_t offset = static_cast<size_t>(row) * stride + static_cast<size_t>(deviceDamage.x()) * 4;
        if (changed || memcmp(view.publishedPixels.data() + offset, bytes + offset, rowLength)) {
            changed = true;
            memcpy(view.publishedPixels.data() + offset, bytes + offset, rowLength);
        }
    }
    if (changed)
        ++view.generation;
#endif
    return SWV_OK;
#endif // USE(COORDINATED_GRAPHICS)
}
} // namespace

extern "C" int32_t scarlet_webview_initialize(SWVBytes fontPath)
{
    if (!validBytes(fontPath, 4096) || !fontPath.length)
        return SWV_INVALID_ARGUMENT;
    // No WTF/JSC call occurs before this lock and the font check. A dedicated
    // owner worker may therefore designate itself as WebCore's main thread.
    pthread_mutex_lock(&initializationMutex);
    if (initialized) {
        auto status = shutDown ? SWV_NOT_INITIALIZED : (pthread_equal(owner, pthread_self()) ? SWV_OK : SWV_WRONG_THREAD);
        pthread_mutex_unlock(&initializationMutex);
        return status;
    }
    std::string path(reinterpret_cast<const char*>(fontPath.data), fontPath.length);
    if (path.find('\0') != std::string::npos) {
        pthread_mutex_unlock(&initializationMutex);
        return SWV_INVALID_ARGUMENT;
    }
    FcConfig* fonts = FcConfigCreate();
    bool validFonts = fonts && FcConfigParseAndLoad(fonts, reinterpret_cast<const FcChar8*>(path.c_str()), FcTrue);
    if (validFonts) {
        // The configured cache may be on a freshly mounted tmpfs. Use Scarlet's
        // supported directory mode before Fontconfig builds its own caches.
        prepareFontCacheDirectories(fonts);
        validFonts = FcConfigBuildFonts(fonts) && FcConfigSetCurrent(fonts);
    }
    if (fonts)
        FcConfigDestroy(fonts);
    if (!validFonts) {
        pthread_mutex_unlock(&initializationMutex);
        return SWV_FAILED;
    }
    owner = pthread_self();
    ScriptController::initializeMainThread();
    if (!WTF::isMainThread()) {
        pthread_mutex_unlock(&initializationMutex);
        return SWV_WRONG_THREAD;
    }
    initializeScarletLiveStrategies();
    ProcessWarming::prewarmGlobally();
    {
        JSC::JSLockHolder locker(commonVM());
        // notifyNeedTermination only signals the VM. Preallocate its real
        // termination exception on the owner before publishing the VM; the
        // minimal embedding does not create a watchdog to do this for us.
        commonVM().ensureTerminationException();
    }
    terminationVM = &commonVM();
    initialized = true;
    pthread_mutex_unlock(&initializationMutex);
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_shutdown()
{
    auto status = ownerStatus();
    if (status != SWV_OK)
        return status;
    if (!views.empty())
        return SWV_FAILED;
    traceTeardown("before-close-storage");
    closeScarletStorage();
    traceTeardown("after-close-storage");
#if USE(SKIA)
    {
        traceTeardown("before-vm-lock");
        JSC::JSLockHolder locker(commonVM());
        traceTeardown("after-vm-lock");
        traceTeardown("before-full-gc");
        commonVM().heap.collectNow(JSC::Sync, JSC::CollectionScope::Full);
        traceTeardown("after-full-gc");
    }
    traceTeardown("after-vm-lock-release");
#endif
    traceTeardown("before-runloop-cycle");
    WTF::RunLoop::cycle();
    traceTeardown("after-runloop-cycle");
    traceTeardown("before-publication-lock");
    pthread_mutex_lock(&initializationMutex);
    terminationVM = nullptr;
    pthread_mutex_unlock(&initializationMutex);
    traceTeardown("after-publication-lock");
    // Native TLS releases m_clientData before its FontCache destructor. Stop
    // the real font timers while the current thread data still exists.
    auto& threadData = threadGlobalDataSingleton();
    traceTeardown("before-thread-data-destroy");
    threadData.destroy();
    traceTeardown("after-thread-data-destroy");
    traceTeardown("before-shared-timer-stop");
    threadData.threadTimers().setSharedTimer(nullptr);
    traceTeardown("after-shared-timer-stop");
#if USE(SKIA)
    traceTeardown("before-gpu-context-destroy");
    PlatformDisplay::destroySharedDisplay();
    traceTeardown("after-gpu-context-destroy");
#endif
    shutDown = true;
    traceTeardown("shutdown-complete");
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_request_termination()
{
    // This is the sole cross-thread ABI. Never discover/create a VM here: the
    // owner publishes its existing VM and revokes it before native teardown.
    bool diagnostics = std::getenv("SCARLET_GC_DIAGNOSTICS");
    if (diagnostics)
        std::printf("SCARLET_WEBVIEW_TERMINATION stage=before-publication-lock\n");
    pthread_mutex_lock(&initializationMutex);
    if (!initialized.load(std::memory_order_acquire) || shutDown.load(std::memory_order_acquire) || !terminationVM) {
        pthread_mutex_unlock(&initializationMutex);
        return SWV_NOT_INITIALIZED;
    }
    if (diagnostics)
        std::printf("SCARLET_WEBVIEW_TERMINATION stage=before-notify vm=%p trap_bits=%p pending=%u entries=%llu\n",
            terminationVM, terminationVM->traps().trapBitsAddress(),
            terminationVM->traps().hasTrapBit(JSC::VMTraps::NeedTermination),
            static_cast<unsigned long long>(scarlet_jsc_execution_entry_count()));
    terminationVM->notifyNeedTermination();
    if (diagnostics)
        std::printf("SCARLET_WEBVIEW_TERMINATION stage=after-notify vm=%p pending=%u\n",
            terminationVM, terminationVM->traps().hasTrapBit(JSC::VMTraps::NeedTermination));
    pthread_mutex_unlock(&initializationMutex);
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_create(const SWVConfiguration* configuration, ScarletWebView** out)
{
    if (out)
        *out = nullptr;
    auto status = ownerStatus();
    if (status != SWV_OK)
        return status;
    if (!configuration || !out || configuration->abi_version != SCARLET_WEB_VIEW_ABI_VERSION
        || configuration->script_enabled > 1 || !validSize(configuration->width, configuration->height, configuration->device_scale_factor))
        return SWV_INVALID_ARGUMENT;
    auto view = std::make_unique<ScarletWebView>();
    view->width = configuration->width;
    view->height = configuration->height;
    view->scale = configuration->device_scale_factor;
    screenForView(*view);
    auto pageConfiguration = pageConfigurationWithEmptyClients(PageIdentifier::generate(), PAL::SessionID::defaultSessionID());
    pageConfiguration.loadsSubresources = true;
    pageConfiguration.cookieJar = createScarletCookieJar();
    pageConfiguration.databaseProvider = createScarletDatabaseProvider();
    pageConfiguration.storageNamespaceProvider = createScarletStorageNamespaceProvider();
    pageConfiguration.storageNamespaceProvider->setSessionStorageQuota(5 * 1024 * 1024);
    pageConfiguration.editorClient = createScarletEditorClient();
#if USE(COORDINATED_GRAPHICS)
    view->layerTree = ScarletLayerTree::create([state = &view->loadState] {
        // Composition-only changes do not invalidate DOM backing tiles.
        state->needsRenderingUpdate = true;
    });
    if (!view->layerTree)
        return SWV_FAILED;
    view->layerTree->setViewport(cssSize(*view), static_cast<float>(view->scale));
    pageConfiguration.chromeClient = createScarletChromeClient(view->loadState, *view->layerTree);
#else
    pageConfiguration.chromeClient = createScarletChromeClient(view->loadState);
#endif
    pageConfiguration.backForwardClient = createScarletBackForwardClient();
    auto* parameters = std::get_if<PageConfiguration::LocalMainFrameCreationParameters>(&pageConfiguration.mainFrameCreationParameters);
    if (!parameters)
        return SWV_FAILED;
    // This is the embedder's sandbox, independent of HTML iframe/CSP sandbox.
    // A normal live browsing context must retain its genuine network origin.
    parameters->effectiveSandboxFlags = { };
    parameters->clientCreator = [state = &view->loadState](LocalFrame& frame, FrameLoader& loader) -> UniqueRef<LocalFrameLoaderClient> {
        return makeUniqueRefWithoutRefCountedCheck<ScarletFrameLoaderClient>(frame, loader, *state);
    };
    view->page = Page::create(WTF::move(pageConfiguration));
    auto& settings = view->page->settings();
    settings.setScriptEnabled(configuration->script_enabled);
    settings.setImagesEnabled(true);
    settings.setLoadsImagesAutomatically(true);
#if USE(SKIA)
    // Match the standard WebKit embedder preference; Canvas retains its
    // willReadFrequently and minimum-area choices when allocating a buffer.
    settings.setCanvasUsesAcceleratedDrawing(true);
#endif
    settings.setUseUIProcessForBackForwardItemLoading(false);
    settings.setLocalStorageEnabled(true);
    settings.setSessionStorageEnabled(true);
    settings.setCookieEnabled(true);
#if USE(COORDINATED_GRAPHICS)
    settings.setAcceleratedCompositingEnabled(true);
    settings.setForceCompositingMode(true);
    settings.setAcceleratedCompositingForFixedPositionEnabled(true);
    // Split scroll contents into retained layers; input remains on the owner thread.
    settings.setAsyncOverflowScrollingEnabled(true);
#else
    settings.setAcceleratedCompositingEnabled(false);
#endif
#if ENABLE(VIDEO)
    #if USE(SCARLET_MEDIA)
    settings.setMediaEnabled(true);
    settings.setRequestVideoFrameCallbackEnabled(true);
#else
    settings.setMediaEnabled(false);
#endif
#endif
    settings.setStandardFontFamily("serif"_s);
    settings.setSerifFontFamily("serif"_s);
    settings.setSansSerifFontFamily("sans-serif"_s);
    settings.setFixedFontFamily("monospace"_s);
    settings.setDefaultFontSize(16);
    settings.setDefaultFixedFontSize(13);
    view->page->setDeviceScaleFactor(view->scale);
    view->frame = view->page->localMainFrame();
    if (!view->frame)
        return SWV_FAILED;
    registerScarletLivePage(*view->page, view.get());
    view->frame->setView(LocalFrameView::create(*view->frame, cssSize(*view)));
    view->frame->init();
    if (!view->frame->view()) {
        unregisterScarletLivePage(*view->page);
        return SWV_FAILED;
    }
    configureView(*view->frame->view());
    view->page->focusController().setActive(true);
    view->page->focusController().setFocused(true);
    view->page->focusController().setFocusedFrame(view->frame.get());
    auto* pointer = view.release();
    views.insert(pointer);
    *out = pointer;
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_destroy(ScarletWebView* view)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
#if USE(COORDINATED_GRAPHICS)
    discardPendingGpuFrame(*view);
#endif
    traceTeardown("before-stop-loaders");
    view->frame->loader().stopAllLoaders();
    traceTeardown("after-stop-loaders");
    unregisterScarletLivePage(*view->page);
    view->surface = nullptr;
    view->publishedPixels.clear();
#if USE(COORDINATED_GRAPHICS)
    view->layerTree->detach();
#endif
    traceTeardown("before-release-page");
    view->page = nullptr;
    traceTeardown("after-release-page");
    traceTeardown("before-release-frame");
    view->frame = nullptr;
    traceTeardown("after-release-frame");
    views.erase(view);
    traceTeardown("before-delete-view");
    delete view;
    traceTeardown("after-delete-view");
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_navigate(ScarletWebView* view, SWVBytes urlBytes)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!validBytes(urlBytes, 2 * 1024 * 1024) || !urlBytes.length)
        return SWV_INVALID_ARGUMENT;
    auto string = fromBytes(urlBytes);
    URL url { string };
    if (string.isNull() || !url.isValid())
        return SWV_INVALID_ARGUMENT;
    if (!url.protocolIsInHTTPFamily() && !url.protocolIsData() && !url.isAboutBlank())
        return SWV_UNSUPPORTED;
#if USE(COORDINATED_GRAPHICS)
    discardPendingGpuFrame(*view);
#endif
    screenForView(*view);
    view->loadState.error = { };
    view->loadState.needsPaint = true;
    view->loadState.fullPaint = true;
    FrameLoadRequest request { *view->frame, ResourceRequest { WTF::move(url) } };
    request.setIsRequestFromClientOrUserInput();
    view->frame->loader().load(WTF::move(request));
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_resize(ScarletWebView* view, uint32_t width, uint32_t height, double scale)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!validSize(width, height, scale))
        return SWV_INVALID_ARGUMENT;
    if (view->width == width && view->height == height && view->scale == scale)
        return SWV_OK;
#if USE(COORDINATED_GRAPHICS)
    discardPendingGpuFrame(*view);
#endif
    view->width = width;
    view->height = height;
    view->scale = scale;
#if USE(COORDINATED_GRAPHICS)
    for (auto& slot : view->gpuSlots)
        slot.pendingSceneDamage.reset();
    view->layerTree->setViewport(cssSize(*view), static_cast<float>(scale));
#endif
    view->hasMousePosition = false;
    view->surface = nullptr;
    view->publishedPixels.clear();
    view->loadState.needsPaint = true;
    view->loadState.fullPaint = true;
    screenForView(*view);
    view->page->setDeviceScaleFactor(scale);
    if (RefPtr frameView = view->frame->view()) {
        frameView->resize(cssSize(*view));
        frameView->setNeedsLayoutAfterViewConfigurationChange();
    }
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_reload(ScarletWebView* view)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
#if USE(COORDINATED_GRAPHICS)
    discardPendingGpuFrame(*view);
#endif
    view->loadState.needsPaint = true;
    view->loadState.fullPaint = true;
    view->frame->loader().reload({ }, true);
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_back(ScarletWebView* view)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!view->page->backForward().canGoBackOrForward(-1))
        return SWV_EMPTY;
#if USE(COORDINATED_GRAPHICS)
    discardPendingGpuFrame(*view);
#endif
    view->loadState.needsPaint = true;
    view->loadState.fullPaint = true;
    return view->page->backForward().goBack() ? SWV_OK : SWV_FAILED;
}

extern "C" int32_t scarlet_webview_forward(ScarletWebView* view)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!view->page->backForward().canGoBackOrForward(1))
        return SWV_EMPTY;
#if USE(COORDINATED_GRAPHICS)
    discardPendingGpuFrame(*view);
#endif
    view->loadState.needsPaint = true;
    view->loadState.fullPaint = true;
    return view->page->backForward().goForward() ? SWV_OK : SWV_FAILED;
}

extern "C" int32_t scarlet_webview_focus(ScarletWebView* view, uint32_t focused)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (focused > 1)
        return SWV_INVALID_ARGUMENT;
    view->loadState.needsPaint = true;
    view->page->focusController().setActive(focused);
    view->page->focusController().setFocused(focused);
    if (focused)
        view->page->focusController().setFocusedFrame(view->frame.get());
    else {
        // Cancel the native capture/press chain without fabricating a click.
        auto position = IntPoint { static_cast<int>(std::round(view->previousMousePosition.x())), static_cast<int>(std::round(view->previousMousePosition.y())) };
        if (RefPtr frameView = view->frame->view())
            position = frameView->viewToContents(position);
        if (view->pressedButtons || view->frame->eventHandler().mousePressed()) {
            view->page->pointerCaptureController().cancelPointer(mousePointerID, position);
            view->page->pointerCaptureController().clearUnmatchedMouseDown(mousePointerID);
        }
        view->frame->eventHandler().clear();
        if (RefPtr document = view->frame->document())
            document->updateHoverActiveState(HitTestRequest { HitTestSource::User, { HitTestRequest::Type::Release } }, nullptr);
        view->pressedButtons = 0;
        view->hasMousePosition = false;
        PlatformKeyboardEvent::setCurrentModifierState({ });
    }
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_tick(ScarletWebView* view)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
#if USE(SKIA)
    ProfileOperation profile(*view, ProfileOperation::Kind::Tick);
#endif
#if USE(COORDINATED_GRAPHICS)
    if (completePendingGpuFrame(*view) == SWV_FAILED)
        return SWV_FAILED;
#endif
    screenForView(*view);
    {
#if USE(SKIA)
        ProfileTickPhase phase(*view, view->paintProfile.counters.runLoopMicros);
#endif
        WTF::RunLoop::cycle();
    }
    // This runs real animation frame callbacks, timelines, resize/scroll
    // events, observers and rendering steps; the host never executes JS.
    RefPtr frameView = view->frame->view();
#if USE(COORDINATED_GRAPHICS)
    bool performedRenderingUpdate = false;
#endif
    if (view->loadState.needsRenderingUpdate || view->loadState.needsPaint || (frameView && frameView->needsLayout())) {
        // Clear before running callbacks: animation frames and observers may
        // request the next update while this one is being completed.
        view->loadState.needsRenderingUpdate = false;
        {
#if USE(SKIA)
            ProfileTickPhase phase(*view, view->paintProfile.counters.renderingUpdateMicros);
#endif
            view->page->updateRendering();
        }
        {
#if USE(SKIA)
            ProfileTickPhase phase(*view, view->paintProfile.counters.renderingFinalizeMicros);
#endif
            view->page->finalizeRenderingUpdate({ });
        }
#if USE(COORDINATED_GRAPHICS)
        performedRenderingUpdate = true;
#endif
    }
#if USE(COORDINATED_GRAPHICS)
    // Composition-only updates still commit new video buffers and sampled
    // layer properties, without repeating Page's DOM/rendering lifecycle.
    if (performedRenderingUpdate || view->layerTree->needsComposition()) {
        frameView = view->frame->view();
        if (frameView) {
#if USE(SKIA)
            ProfileTickPhase phase(*view, view->paintProfile.counters.sceneFlushMicros);
#endif
            if (!view->layerTree->flush(*frameView))
                view->loadState.needsRenderingUpdate = true;
        }
    }
    if (performedRenderingUpdate)
        view->page->didUpdateRendering();
#endif
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_needs_paint(ScarletWebView* view, uint32_t* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out)
        return SWV_INVALID_ARGUMENT;
    RefPtr frameView = view->frame->view();
    *out = view->loadState.needsPaint || !view->surface || (frameView && frameView->needsLayout())
#if USE(COORDINATED_GRAPHICS)
        || view->pendingGpuFrame.has_value() || view->layerTree->needsComposition()
#endif
        ;
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_pixels(ScarletWebView* view, SWVPixels* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out)
        return SWV_INVALID_ARGUMENT;
    *out = { };
#if USE(SKIA)
    return SWV_UNSUPPORTED;
#else
    status = paint(*view);
    if (status != SWV_OK)
        return status;
    if (!view->surface || view->publishedPixels.empty())
        return SWV_EMPTY;
    auto stride = cairo_image_surface_get_stride(view->surface.get());
    auto* data = view->publishedPixels.data();
    if (!data || stride < static_cast<int>(view->width * 4))
        return SWV_FAILED;
    *out = { data, static_cast<size_t>(stride) * view->height, view->width, view->height, static_cast<uint32_t>(stride), 1, view->generation };
    return SWV_OK;
#endif
}

extern "C" int32_t scarlet_webview_gpu_frame_state(ScarletWebView* view, uint32_t* out)
{
    if (out)
        *out = 0;
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out)
        return SWV_INVALID_ARGUMENT;
#if USE(COORDINATED_GRAPHICS)
    // Pure owner-state observation: do not collect completion, tick or paint.
    if (view->pendingGpuFrame)
        *out = view->pendingGpuFrame->ready ? 2 : 1;
    return SWV_OK;
#else
    return SWV_UNSUPPORTED;
#endif
}

extern "C" int32_t scarlet_webview_gpu_frame_capacity(ScarletWebView* view, uint32_t* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK) return status;
    if (!out) return SWV_INVALID_ARGUMENT;
#if USE(SKIA)
    *out = 0;
#if USE(COORDINATED_GRAPHICS)
    if (view->pendingGpuFrame) {
        auto completion = completePendingGpuFrame(*view);
        if (completion == SWV_FAILED)
            return completion;
        *out = view->pendingGpuFrame->ready ? 1 : 0;
        return SWV_OK;
    }
#endif
    for (const auto& slot : view->gpuSlots) if (!slot.lease) ++*out;
    return SWV_OK;
#else
    return SWV_UNSUPPORTED;
#endif
}
extern "C" int32_t scarlet_webview_gpu_frame(ScarletWebView* view, SWVGpuFrame* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK) return status;
    if (!out) return SWV_INVALID_ARGUMENT;
    *out = { -1, 0, 0, 0, 0 };
#if USE(SKIA)
#if USE(COORDINATED_GRAPHICS)
    if (view->pendingGpuFrame) {
        status = completePendingGpuFrame(*view);
        if (status != SWV_OK)
            return status;
        auto& pending = *view->pendingGpuFrame;
        auto& slot = *pending.target;
        auto handle = slot.surface->duplicateHandle();
        if (handle < 0)
            return SWV_FAILED;
        slot.lease = pending.generation;
        *out = { handle, slot.width, slot.height, 1, slot.lease };
        view->pendingGpuFrame.reset();
        return SWV_OK;
    }
#endif
    RefPtr frameView = view->frame->view();
    if (view->surface && !view->loadState.needsPaint && frameView && !frameView->needsLayout()
#if USE(COORDINATED_GRAPHICS)
        && !view->layerTree->needsComposition()
#endif
    ) return SWV_EMPTY;
    auto device = PlatformDisplay::sharedDisplay().skiaVulkanContext();
    if (!device) { view->loadState.error = "Native Vulkan context creation failed."_s; return SWV_FAILED; }
    for (auto& slot : view->gpuSlots) {
        if (slot.lease) continue;
        if (!slot.surface || slot.width != view->width || slot.height != view->height) {
            if (view->nextImageIdentifier == UINT64_MAX) {
                view->loadState.error = "Native GPU image identifiers exhausted."_s;
                return SWV_FAILED;
            }
            slot.imageIdentifier = 0;
            slot.surface = nullptr;
            slot.surface = device->createSurface(view->width, view->height);
            if (!slot.surface) { view->loadState.error = "Native shared page image allocation failed."_s; return SWV_FAILED; }
            slot.imageIdentifier = ++view->nextImageIdentifier;
            slot.pendingFullPaint = true;
            slot.pendingDamage = { };
#if USE(COORDINATED_GRAPHICS)
            slot.pendingSceneDamage.reset();
#endif
            slot.width = view->width; slot.height = view->height;
        }
        view->surface = slot.surface.get();
        status = paint(*view);
        if (status != SWV_OK) return status;
        auto handle = slot.surface->duplicateHandle();
        if (handle < 0) return SWV_FAILED;
        slot.lease = view->generation;
        *out = { handle, slot.width, slot.height, 1, slot.lease };
        return SWV_OK;
    }
    return SWV_EMPTY;
#else
    return SWV_UNSUPPORTED;
#endif
}
extern "C" int32_t scarlet_webview_gpu_frame_image_id(ScarletWebView* view, uint64_t generation, uint64_t* out)
{
    if (out) *out = 0;
    auto status = viewStatus(view);
    if (status != SWV_OK) return status;
    if (!out || !generation) return SWV_INVALID_ARGUMENT;
#if USE(SKIA)
    for (const auto& slot : view->gpuSlots) {
        if (slot.lease != generation) continue;
        if (!slot.surface || !slot.imageIdentifier) return SWV_FAILED;
        *out = slot.imageIdentifier;
        return SWV_OK;
    }
    return SWV_STALE_REQUEST;
#else
    return SWV_UNSUPPORTED;
#endif
}

extern "C" int32_t scarlet_webview_release_gpu_frame(ScarletWebView* view, uint64_t generation)
{
    auto status = viewStatus(view);
    if (status != SWV_OK) return status;
#if USE(SKIA)
    if (!generation) return SWV_INVALID_ARGUMENT;
    for (auto& slot : view->gpuSlots) {
        if (slot.lease != generation) continue;
        slot.lease = 0;
        return SWV_OK;
    }
    return SWV_STALE_REQUEST;
#else
    return SWV_UNSUPPORTED;
#endif
}

extern "C" int32_t scarlet_webview_gpu_frame_readback(ScarletWebView* view, uint64_t generation, SWVPixels* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK) return status;
    if (!out || !generation) return SWV_INVALID_ARGUMENT;
    *out = { };
#if USE(SKIA)
    for (const auto& slot : view->gpuSlots) {
        if (slot.lease != generation) continue;
        auto info = SkImageInfo::Make(slot.width, slot.height, kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
        auto stride = info.minRowBytes();
        view->publishedPixels.resize(info.computeMinByteSize());
        if (!slot.surface->skSurface().readPixels(info, view->publishedPixels.data(), stride, 0, 0)) return SWV_FAILED;
        *out = { view->publishedPixels.data(), view->publishedPixels.size(), slot.width, slot.height, static_cast<uint32_t>(stride), 1, generation };
        return SWV_OK;
    }
    return SWV_STALE_REQUEST;
#else
    return SWV_UNSUPPORTED;
#endif
}

extern "C" int32_t scarlet_webview_state(ScarletWebView* view, SWVState* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out)
        return SWV_INVALID_ARGUMENT;
    RefPtr document = view->frame->document();
    view->url = document ? toUTF8(document->url().string()) : std::string { };
    view->title = document ? toUTF8(document->title()) : std::string { };
    view->error = toUTF8(view->loadState.error);
    RefPtr frameView = view->frame->view();
    *out = { borrow(view->url), borrow(view->title), borrow(view->error), view->generation,
        scarlet_jsc_execution_entry_count(), view->loadState.loading,
        pendingScarletLiveRequests(*view->page), frameView ? frameView->scrollX() : 0, frameView ? frameView->scrollY() : 0,
        view->page->backForward().canGoBackOrForward(-1), view->page->backForward().canGoBackOrForward(1) };
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_mouse(ScarletWebView* view, const SWVMouseEvent* input, uint32_t* handled)
{
    if (handled) *handled = 0;
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!input || !handled || input->kind > SWV_MOUSE_UP || input->button > SWV_BUTTON_RIGHT
        || !validModifiers(input->modifiers) || input->click_count > 3 || !std::isfinite(input->x) || !std::isfinite(input->y)
        || !std::isfinite(input->global_x) || !std::isfinite(input->global_y)
        || std::abs(input->x) > 1000000 || std::abs(input->y) > 1000000
        || std::abs(input->global_x) > 1000000 || std::abs(input->global_y) > 1000000
        || (input->kind != SWV_MOUSE_MOVE && (input->button == SWV_BUTTON_NONE || !input->click_count)))
        return SWV_INVALID_ARGUMENT;
    constexpr MouseButton buttons[] = { MouseButton::None, MouseButton::Left, MouseButton::Middle, MouseButton::Right };
    constexpr PlatformEvent::Type types[] = { PlatformEvent::Type::MouseMoved, PlatformEvent::Type::MousePressed, PlatformEvent::Type::MouseReleased };
    auto position = DoublePoint { input->x / view->scale, input->y / view->scale };
    auto delta = view->hasMousePosition
        ? DoublePoint { position.x() - view->previousMousePosition.x(), position.y() - view->previousMousePosition.y() }
        : DoublePoint { };
    view->previousMousePosition = position;
    view->hasMousePosition = true;
    constexpr unsigned short buttonMasks[] = { 0, 1, 4, 2 };
    if (input->kind == SWV_MOUSE_DOWN)
        view->pressedButtons |= buttonMasks[input->button];
    else if (input->kind == SWV_MOUSE_UP)
        view->pressedButtons &= ~buttonMasks[input->button];
    PlatformMouseEvent event { position,
        DoublePoint { input->global_x / view->scale, input->global_y / view->scale }, buttons[input->button], types[input->kind],
        static_cast<int>(input->click_count), modifiers(input->modifiers), MonotonicTime::now(), 0, SyntheticClickType::NoTap, MouseEventInputSource::UserDriven,
        mousePointerID, delta };
    event.setButtons(view->pressedButtons);
    // Hover/selection/DOM changes invalidate through ChromeClient. Merely
    // moving the pointer must not repaint the entire pixel surface.
    view->loadState.needsRenderingUpdate = true;
    auto& handler = view->frame->eventHandler();
    auto result = input->kind == SWV_MOUSE_MOVE ? handler.mouseMoved(event)
        : input->kind == SWV_MOUSE_DOWN ? handler.handleMousePressEvent(event) : handler.handleMouseReleaseEvent(event);
    *handled = result.wasHandled();
    return result.remoteUserInputEventData() ? SWV_UNSUPPORTED : SWV_OK;
}

namespace {
class ScarletWheelEvent final : public PlatformWheelEvent {
public:
    ScarletWheelEvent(const SWVWheelEvent& input, double scale)
    {
        m_type = Type::Wheel;
        m_modifiers = ::modifiers(input.modifiers);
        m_timestamp = MonotonicTime::now();
        m_position = { static_cast<int>(std::round(input.x / scale)), static_cast<int>(std::round(input.y / scale)) };
        m_globalPosition = { static_cast<int>(std::round(input.global_x / scale)), static_cast<int>(std::round(input.global_y / scale)) };
        m_deltaX = input.delta_x / scale;
        m_deltaY = input.delta_y / scale;
        m_wheelTicksX = m_deltaX / 40;
        m_wheelTicksY = m_deltaY / 40;
        m_hasPreciseScrollingDeltas = true;
    }
};
}

extern "C" int32_t scarlet_webview_wheel(ScarletWebView* view, const SWVWheelEvent* input, uint32_t* handled)
{
    if (handled) *handled = 0;
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!input || !handled || !validModifiers(input->modifiers) || !std::isfinite(input->x) || !std::isfinite(input->y)
        || !std::isfinite(input->delta_x) || !std::isfinite(input->delta_y)
        || !std::isfinite(input->global_x) || !std::isfinite(input->global_y)
        || std::abs(input->global_x) > 1000000 || std::abs(input->global_y) > 1000000
        || std::abs(input->x) > 1000000 || std::abs(input->y) > 1000000
        || std::abs(input->delta_x) > 1000000 || std::abs(input->delta_y) > 1000000)
        return SWV_INVALID_ARGUMENT;
    ScarletWheelEvent event { *input, view->scale };
    view->loadState.needsPaint = true;
    auto result = view->frame->eventHandler().handleWheelEvent(event,
        { WheelEventProcessingSteps::SynchronousScrolling, WheelEventProcessingSteps::BlockingDOMEventDispatch });
    *handled = result.first.wasHandled();
    return result.first.remoteUserInputEventData() ? SWV_UNSUPPORTED : SWV_OK;
}

extern "C" int32_t scarlet_webview_key(ScarletWebView* view, const SWVKeyEvent* input, uint32_t* handled)
{
    if (handled) *handled = 0;
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!input || !handled || input->kind > SWV_KEY_CHAR || !validModifiers(input->modifiers)
        || input->auto_repeat > 1 || input->windows_virtual_key > 65535)
        return SWV_INVALID_ARGUMENT;
    for (auto bytes : { input->text, input->unmodified_text, input->key, input->code, input->key_identifier }) {
        if (!validBytes(bytes, 65536) || fromBytes(bytes).isNull())
            return SWV_INVALID_ARGUMENT;
    }
    constexpr PlatformEvent::Type types[] = { PlatformEvent::Type::RawKeyDown, PlatformEvent::Type::KeyUp, PlatformEvent::Type::Char };
    PlatformKeyboardEvent event { types[input->kind], fromBytes(input->text), fromBytes(input->unmodified_text),
        fromBytes(input->key), fromBytes(input->code), fromBytes(input->key_identifier), static_cast<int>(input->windows_virtual_key),
        !!input->auto_repeat, false, false, modifiers(input->modifiers), MonotonicTime::now() };
    PlatformKeyboardEvent::setCurrentModifierState(modifiers(input->modifiers));
    view->loadState.needsRenderingUpdate = true;
    *handled = view->frame->eventHandler().keyEvent(event);
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_poll_request(ScarletWebView* view, SWVRequest* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out)
        return SWV_INVALID_ARGUMENT;
    *out = { };
    return pollScarletLiveRequest(*view->page, *out);
}

extern "C" int32_t scarlet_webview_complete_request(ScarletWebView* view, const SWVResponse* response)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!response)
        return SWV_INVALID_ARGUMENT;
#if USE(SKIA)
    ProfileOperation profile(*view, ProfileOperation::Kind::CompleteRequest);
#endif
    auto result = completeScarletLiveRequest(*view->page, *response);
    if (result == SWV_OK || result == SWV_FAILED)
        view->loadState.needsRenderingUpdate = true;
    return result;
}

#if ENABLE(VIDEO)
static void reportMediaProfile(ScarletWebView& view, HTMLVideoElement& video, uint32_t index)
{
    auto& profile = view.mediaProfile;
    if (!profile.enabled)
        return;
    auto now = MonotonicTime::now();
    auto currentSrc = video.currentSrc().string();
    auto src = video.getURLAttribute(HTMLNames::srcAttr).string();
    RefPtr error = video.error();
    unsigned errorCode = error ? static_cast<unsigned>(error->code()) : 0;
    String errorMessage = error ? error->message() : String { };
    bool changed = !profile.reported || profile.index != index || profile.currentSrc != currentSrc
        || profile.src != src || profile.errorCode != errorCode || profile.errorMessage != errorMessage;
    if (!changed && (now - profile.lastReport).seconds() < 5.0)
        return;
    profile.reported = true;
    profile.index = index;
    profile.lastReport = now;
    profile.currentSrc = currentSrc;
    profile.src = src;
    profile.errorCode = errorCode;
    profile.errorMessage = errorMessage;

    auto state = JSON::Object::create();
    state->setDouble("index"_s, index);
    state->setString("pageUrl"_s, video.document().url().string());
    state->setString("currentSrc"_s, currentSrc);
    state->setString("src"_s, src);
    if (error) {
        auto details = JSON::Object::create();
        details->setInteger("code"_s, static_cast<int>(errorCode));
        details->setString("message"_s, errorMessage);
        state->setObject("error"_s, WTF::move(details));
    } else
        state->setValue("error"_s, JSON::Value::null());
    auto buffered = JSON::Array::create();
    auto ranges = video.buffered();
    for (unsigned i = 0; i < ranges->length(); ++i) {
        auto range = JSON::Array::create();
        range->pushDouble(ranges->ranges().start(i).toDouble());
        range->pushDouble(ranges->ranges().end(i).toDouble());
        buffered->pushArray(WTF::move(range));
    }
    state->setArray("buffered"_s, WTF::move(buffered));
    state->setBoolean("paused"_s, video.paused());
    state->setBoolean("ended"_s, video.ended());
    state->setBoolean("seeking"_s, video.seeking());
    state->setInteger("networkState"_s, static_cast<int>(video.networkState()));
    state->setInteger("readyState"_s, static_cast<int>(video.readyState()));
    state->setDouble("playbackRate"_s, video.playbackRate());
    state->setDouble("currentTime"_s, video.currentTime());
    state->setDouble("duration"_s, video.duration());
    auto json = state->toJSONString().utf8();
    std::fprintf(stderr, "SCARLET_WEBVIEW_PROFILE stage=video-state view=%p data=%s\n",
        static_cast<void*>(&view), reinterpret_cast<const char*>(json.data()));
}
#endif

extern "C" int32_t scarlet_webview_media_statistics(ScarletWebView* view, uint32_t index, ScarletWebViewMediaStatistics* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out)
        return SWV_INVALID_ARGUMENT;
    *out = { };
#if ENABLE(VIDEO)
    RefPtr video = mediaElementAt(*view, index, out->video_count);
    if (!out->video_count)
        return SWV_EMPTY;
    if (!video)
        return SWV_INVALID_ARGUMENT;
    reportMediaProfile(*view, *video, index);
    collectMediaCounters(*video, *out);
    auto rectangle = video->getBoundingClientRect();
    out->rect_x = rectangle->x();
    out->rect_y = rectangle->y();
    out->rect_width = rectangle->width();
    out->rect_height = rectangle->height();
    return SWV_OK;
#else
    return SWV_UNSUPPORTED;
#endif
}

extern "C" int32_t scarlet_webview_compositing_statistics(ScarletWebView* view, SWVCompositingStatistics* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out)
        return SWV_INVALID_ARGUMENT;
    *out = { };
#if USE(COORDINATED_GRAPHICS)
    auto statistics = view->layerTree->tilePaintStatistics();
    out->tile_paints = statistics.paints;
    out->tile_pixels = statistics.pixels;
    out->enabled = 1;
    return SWV_OK;
#else
    return SWV_UNSUPPORTED;
#endif
}

extern "C" int32_t scarlet_webview_paint_statistics(ScarletWebView* view, SWVPaintStatistics* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out)
        return SWV_INVALID_ARGUMENT;
    *out = view->paintStatistics;
    return SWV_OK;
}

extern "C" int32_t scarlet_webview_text_content(ScarletWebView* view, SWVBytes id, SWVBytes* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out || !validBytes(id, 1024))
        return SWV_INVALID_ARGUMENT;
    *out = { };
    auto identifier = fromBytes(id);
    if (identifier.isNull())
        return SWV_INVALID_ARGUMENT;
    RefPtr document = view->frame->document();
    if (!document)
        return SWV_EMPTY;
    auto element = document->getElementById(identifier);
    if (!element)
        return SWV_EMPTY;
    view->inspectedText = toUTF8(element->textContent());
    *out = borrow(view->inspectedText);
    return SWV_OK;
}
