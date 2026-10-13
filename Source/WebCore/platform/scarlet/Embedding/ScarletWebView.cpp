// Native persistent WebCore Page embedding. Every call belongs to one owner
// worker. HTTP work is cloned by the host and completed back on that worker.
#include "config.h"
#include "ScarletWebView.h"
#include "ScarletEditorClient.h"
#include "ScarletChromeClient.h"
#include "ScarletBackForwardClient.h"
#include "ScarletFrameLoaderClient.h"
#include "ScarletLiveLoader.h"
#include "Color.h"
#include "BackForwardController.h"
#include "CommonVM.h"
#include "Document.h"
#include "DocumentView.h"
#include "Element.h"
#include "EmptyClients.h"
#include "EventHandler.h"
#include "FocusController.h"
#include "FrameLoadRequest.h"
#include "FrameLoader.h"
#include "GraphicsContextCairo.h"
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
#include "RefPtrCairo.h"
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
#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#if !PLATFORM(SCARLET) || !OS(SCARLET) || !USE(CAIRO) || !ENABLE(C_LOOP) || ENABLE(JIT)
#error "ScarletWebView requires native Scarlet Cairo and the C-loop JavaScript interpreter"
#endif
extern "C" uint64_t scarlet_jsc_execution_entry_count();

using namespace WebCore;
using namespace WTF::StringLiterals;

struct ScarletWebView {
    ScarletFrameLoadState loadState;
    RefPtr<Page> page;
    RefPtr<LocalFrame> frame;
    RefPtr<cairo_surface_t> surface;
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
    std::vector<uint8_t> publishedPixels;
};

namespace {
pthread_mutex_t initializationMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t owner;
std::atomic<bool> initialized { false };
std::atomic<bool> shutDown { false };
JSC::VM* terminationVM { nullptr }; // Published/revoked under initializationMutex.
std::unordered_set<ScarletWebView*> views;

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
        auto size = cssSize(view);
        context.fillRect(FloatRect { FloatPoint { }, FloatSize { size } }, Color::white);
        // ScrollView::paint applies the actual scroll offset and viewport clip;
        // paintContents alone would incorrectly redraw the document's top.
        frameView->paint(context, IntRect { IntPoint { }, size });
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
    if (view.publishedPixels.size() != length || memcmp(view.publishedPixels.data(), bytes, length)) {
        view.publishedPixels.assign(bytes, bytes + length);
        ++view.generation;
    }
    view.loadState.needsPaint = false;
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
    bool validFonts = fonts && FcConfigParseAndLoad(fonts, reinterpret_cast<const FcChar8*>(path.c_str()), FcTrue)
        && FcConfigBuildFonts(fonts) && FcConfigSetCurrent(fonts);
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
    WTF::RunLoop::cycle();
    pthread_mutex_lock(&initializationMutex);
    terminationVM = nullptr;
    pthread_mutex_unlock(&initializationMutex);
    // Native TLS releases m_clientData before its FontCache destructor. Stop
    // the real font timers while the current thread data still exists.
    auto& threadData = threadGlobalDataSingleton();
    threadData.destroy();
    threadData.threadTimers().setSharedTimer(nullptr);
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
    // Empty page providers are not a storage implementation. Disable these
    // APIs so WebCore reports their unavailability rather than accepting
    // writes that an empty provider would silently discard.
    settings.setLocalStorageEnabled(false);
    settings.setSessionStorageEnabled(false);
    settings.setCookieEnabled(false);
    settings.setAcceleratedCompositingEnabled(false);
#if ENABLE(VIDEO)
    settings.setMediaEnabled(false);
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
    return paint(*view);
}

extern "C" int32_t scarlet_webview_pixels(ScarletWebView* view, SWVPixels* out)
{
    auto status = viewStatus(view);
    if (status != SWV_OK)
        return status;
    if (!out)
        return SWV_INVALID_ARGUMENT;
    *out = { };
    if (!view->surface || view->publishedPixels.empty())
        return SWV_EMPTY;
    auto stride = cairo_image_surface_get_stride(view->surface.get());
    auto* data = view->publishedPixels.data();
    if (!data || stride < static_cast<int>(view->width * 4))
        return SWV_FAILED;
    *out = { data, static_cast<size_t>(stride) * view->height, view->width, view->height, static_cast<uint32_t>(stride), 1, view->generation };
    return SWV_OK;
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
    view->loadState.needsPaint = true;
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
    view->loadState.needsPaint = true;
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
        view->loadState.needsPaint = true;
    return result;
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
