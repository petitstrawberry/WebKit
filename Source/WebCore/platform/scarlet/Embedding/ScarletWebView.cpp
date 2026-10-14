// Native persistent WebCore Page embedding. Every call belongs to one owner
// worker. HTTP work is cloned by the host and completed back on that worker.
#include "config.h"
#include "ScarletWebView.h"
#include "ScarletEditorClient.h"
#include "ScarletChromeClient.h"
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
#else
#include "GraphicsContextCairo.h"
#endif
#include "HandleUserInputEventResult.h"
#include "HitTestRequest.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "LocalFrameView.h"
#include "Page.h"
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

#if !PLATFORM(SCARLET) || !OS(SCARLET) || (!USE(CAIRO) && !USE(SKIA)) || !ENABLE(C_LOOP) || ENABLE(JIT)
#error "ScarletWebView requires native Scarlet graphics and the C-loop JavaScript interpreter"
#endif
extern "C" uint64_t scarlet_jsc_execution_entry_count();

using namespace WebCore;
using namespace WTF::StringLiterals;

struct ScarletWebView {
    ScarletFrameLoadState loadState;
    RefPtr<Page> page;
    RefPtr<LocalFrame> frame;
#if USE(SKIA)
    struct GpuSlot {
        std::unique_ptr<ScarletSkiaVulkan::Surface> surface;
        uint64_t lease { 0 };
        uint32_t width { 0 }, height { 0 };
    };
    std::array<GpuSlot, 2> gpuSlots;
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
    std::vector<uint8_t> publishedPixels;
};

namespace {
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
int32_t paint(ScarletWebView& view)
{
    RefPtr frameView = view.frame->view();
    RefPtr document = view.frame->document();
    if (!frameView || !document)
        return SWV_EMPTY;
    if (!view.loadState.needsPaint && !frameView->needsLayout() && view.surface)
        return SWV_OK;
    document->updateLayoutIgnorePendingStylesheets();
    if (!frameView->renderView())
        return SWV_EMPTY;
    if (frameView->needsLayout() || !frameView->layoutContext().inPaintableState())
        return SWV_EMPTY;

    auto size = cssSize(view);
    IntRect damage { IntPoint { }, size };
#if !USE(SKIA)
    if (view.surface && !view.loadState.fullPaint && !view.loadState.dirtyRect.isEmpty())
        damage.intersect(view.loadState.dirtyRect);
#endif
    view.loadState.needsPaint = false;
    view.loadState.fullPaint = false;
    view.loadState.dirtyRect = { };
    if (damage.isEmpty())
        return SWV_OK;
#if USE(SKIA)
    if (!view.surface) return SWV_FAILED;
    {
        GraphicsContextSkia context(*view.surface->skSurface().getCanvas(), RenderingMode::Accelerated, RenderingPurpose::DOM);
        context.save();
        context.scale(FloatSize { static_cast<float>(view.scale), static_cast<float>(view.scale) });
        context.clip(FloatRect { damage });
        context.fillRect(FloatRect { damage }, Color::white);
        frameView->paint(context, damage);
        context.restore();
    }
    auto device = PlatformDisplay::sharedDisplay().skiaVulkanContext();
    if (!device || !device->submit(*view.surface)) {
        view.loadState.error = "WebCore Vulkan page submission failed."_s;
        return SWV_FAILED;
    }
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
    closeScarletStorage();
#if USE(SKIA)
    {
        JSC::JSLockHolder locker(commonVM());
        commonVM().heap.collectNow(JSC::Sync, JSC::CollectionScope::Full);
    }
#endif
    WTF::RunLoop::cycle();
    pthread_mutex_lock(&initializationMutex);
    terminationVM = nullptr;
    pthread_mutex_unlock(&initializationMutex);
    // Native TLS releases m_clientData before its FontCache destructor. Stop
    // the real font timers while the current thread data still exists.
    auto& threadData = threadGlobalDataSingleton();
    threadData.destroy();
    threadData.threadTimers().setSharedTimer(nullptr);
#if USE(SKIA)
    PlatformDisplay::destroySharedDisplay();
#endif
    shutDown = true;
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
    auto pageConfiguration = pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    pageConfiguration.loadsSubresources = true;
    pageConfiguration.cookieJar = createScarletCookieJar();
    pageConfiguration.databaseProvider = createScarletDatabaseProvider();
    pageConfiguration.storageNamespaceProvider = createScarletStorageNamespaceProvider();
    pageConfiguration.storageNamespaceProvider->setSessionStorageQuota(5 * 1024 * 1024);
    pageConfiguration.editorClient = createScarletEditorClient();
    pageConfiguration.chromeClient = createScarletChromeClient(view->loadState);
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
    settings.setUseUIProcessForBackForwardItemLoading(false);
    settings.setLocalStorageEnabled(true);
    settings.setSessionStorageEnabled(true);
    settings.setCookieEnabled(true);
    settings.setAcceleratedCompositingEnabled(false);
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
    view->frame->loader().stopAllLoaders();
    unregisterScarletLivePage(*view->page);
    view->surface = nullptr;
    view->publishedPixels.clear();
    view->page = nullptr;
    view->frame = nullptr;
    views.erase(view);
    delete view;
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
    screenForView(*view);
    view->loadState.error = { };
    view->loadState.needsPaint = true;
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
    view->width = width;
    view->height = height;
    view->scale = scale;
    view->hasMousePosition = false;
    view->surface = nullptr;
    view->publishedPixels.clear();
    view->loadState.needsPaint = true;
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
    view->loadState.needsPaint = true;
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
    view->loadState.needsPaint = true;
    return view->page->backForward().goBack() ? SWV_OK : SWV_FAILED;
}

extern "C" int32_t scarlet_webview_forward(ScarletWebView* view)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!view->page->backForward().canGoBackOrForward(1))
        return SWV_EMPTY;
    view->loadState.needsPaint = true;
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
    screenForView(*view);
    WTF::RunLoop::cycle();
    // This runs real animation frame callbacks, timelines, resize/scroll
    // events, observers and rendering steps; the host never executes JS.
    RefPtr frameView = view->frame->view();
    if (view->loadState.needsRenderingUpdate || view->loadState.needsPaint || (frameView && frameView->needsLayout())) {
        // Clear before running callbacks: animation frames and observers may
        // request the next update while this one is being completed.
        view->loadState.needsRenderingUpdate = false;
        view->page->updateRendering();
        view->page->finalizeRenderingUpdate({ });
    }
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
    *out = view->loadState.needsPaint || !view->surface || (frameView && frameView->needsLayout());
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

extern "C" int32_t scarlet_webview_gpu_frame_capacity(ScarletWebView* view, uint32_t* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK) return status;
    if (!out) return SWV_INVALID_ARGUMENT;
#if USE(SKIA)
    *out = 0;
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
    RefPtr frameView = view->frame->view();
    if (view->surface && !view->loadState.needsPaint && frameView && !frameView->needsLayout()) return SWV_EMPTY;
    auto device = PlatformDisplay::sharedDisplay().skiaVulkanContext();
    if (!device) { view->loadState.error = "Native Vulkan context creation failed."_s; return SWV_FAILED; }
    for (auto& slot : view->gpuSlots) {
        if (slot.lease) continue;
        if (!slot.surface || slot.width != view->width || slot.height != view->height) {
            slot.surface = nullptr;
            slot.surface = device->createSurface(view->width, view->height);
            if (!slot.surface) { view->loadState.error = "Native shared page image allocation failed."_s; return SWV_FAILED; }
            slot.width = view->width; slot.height = view->height;
        }
        view->surface = slot.surface.get();
        view->loadState.needsPaint = true; // This free target may contain an older revision.
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
    auto result = completeScarletLiveRequest(*view->page, *response);
    if (result == SWV_OK || result == SWV_FAILED)
        view->loadState.needsRenderingUpdate = true;
    return result;
}

extern "C" int32_t scarlet_webview_media_statistics(ScarletWebView* view, uint32_t index, ScarletWebViewMediaStatistics* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out)
        return SWV_INVALID_ARGUMENT;
    *out = { };
#if ENABLE(VIDEO)
    RefPtr document = view->frame->document();
    if (!document)
        return SWV_EMPTY;
    auto elements = document->getElementsByTagName(AtomString { "video"_s });
    RefPtr<HTMLVideoElement> video;
    for (unsigned i = 0; i < elements->length(); ++i) {
        RefPtr candidate = dynamicDowncast<HTMLVideoElement>(elements->item(i));
        if (!candidate)
            continue;
        if (out->video_count == index)
            video = candidate;
        ++out->video_count;
    }
    if (!out->video_count)
        return SWV_EMPTY;
    if (!video)
        return SWV_INVALID_ARGUMENT;
    out->width = video->videoWidth();
    out->height = video->videoHeight();
    out->paused = video->paused();
    out->ended = video->ended();
    out->ready_state = static_cast<uint32_t>(video->readyState());
    out->current_time = video->currentTime();
    out->duration = video->duration();
    auto rectangle = video->getBoundingClientRect();
    out->rect_x = rectangle->x();
    out->rect_y = rectangle->y();
    out->rect_width = rectangle->width();
    out->rect_height = rectangle->height();
    if (RefPtr player = video->player()) {
        out->decoded_frames = player->decodedFrameCount();
        out->dropped_frames = player->droppedFrameCount();
        if (auto metrics = player->videoPlaybackQualityMetrics())
            out->presented_frames = metrics->displayCompositedVideoFrames;
    }
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
