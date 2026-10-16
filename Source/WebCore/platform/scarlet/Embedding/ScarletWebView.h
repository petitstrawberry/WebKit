/* Persistent WebCore embedding ABI. No subprocess, serialized DOM, or host ABI. */
#ifndef SCARLET_WEB_VIEW_H
#define SCARLET_WEB_VIEW_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SCARLET_WEB_VIEW_ABI_VERSION 1u
typedef struct ScarletWebView ScarletWebView;
typedef struct { const uint8_t* data; size_t length; } SWVBytes;
typedef struct { SWVBytes name; SWVBytes value; } SWVHeader;
enum { SWV_OK = 0, SWV_EMPTY = 1, SWV_INVALID_ARGUMENT = -1, SWV_WRONG_THREAD = -2,
    SWV_NOT_INITIALIZED = -3, SWV_UNSUPPORTED = -4, SWV_FAILED = -5, SWV_STALE_REQUEST = -6 };
enum { SWV_SHIFT = 1u, SWV_CONTROL = 2u, SWV_ALT = 4u, SWV_META = 8u, SWV_CAPS_LOCK = 16u };
enum { SWV_MOUSE_MOVE = 0, SWV_MOUSE_DOWN = 1, SWV_MOUSE_UP = 2 };
enum { SWV_BUTTON_NONE = 0, SWV_BUTTON_LEFT = 1, SWV_BUTTON_MIDDLE = 2, SWV_BUTTON_RIGHT = 3 };
enum { SWV_KEY_DOWN = 0, SWV_KEY_UP = 1, SWV_KEY_CHAR = 2 };

typedef struct {
    uint32_t abi_version;
    uint32_t width;
    uint32_t height;
    uint32_t script_enabled;
    double device_scale_factor;
} SWVConfiguration; /* width/height/input coordinates are physical pixels; scale=.25..8. */
typedef struct {
    const uint8_t* data; size_t length;
    uint32_t width; uint32_t height; uint32_t stride; uint32_t format;
    uint64_t generation;
} SWVPixels; /* format=1: little-endian premultiplied BGRA8, opaque page base. */
typedef struct {
    SWVBytes url; SWVBytes title; SWVBytes last_error;
    uint64_t frame_generation; uint64_t script_execution_entries; /* Process-wide native C-loop counter. */
    uint32_t loading; uint32_t pending_requests;
    int32_t scroll_x; int32_t scroll_y;
    uint32_t can_go_back; uint32_t can_go_forward;
} SWVState;
typedef struct {
    uint32_t kind; uint32_t button; uint32_t modifiers; uint32_t click_count;
    double x; double y; double global_x; double global_y;
} SWVMouseEvent;
typedef struct {
    uint32_t modifiers; double x; double y; double delta_x; double delta_y;
    double global_x; double global_y;
} SWVWheelEvent; /* Positive deltas move contents left/up (WebCore convention). */
typedef struct {
    uint32_t kind; uint32_t modifiers; uint32_t windows_virtual_key; uint32_t auto_repeat;
    SWVBytes text; SWVBytes unmodified_text; SWVBytes key; SWVBytes code; SWVBytes key_identifier;
} SWVKeyEvent;
typedef struct {
    uint64_t request_id;
    SWVBytes url; SWVBytes method;
    const SWVHeader* headers; size_t header_count;
    SWVBytes body;
    uint32_t credentials_mode; /* 0=omit, 1=same-origin, 2=include; no automatic cookies in ABI v1. */
    uint32_t is_cancel; /* 1 cancels a previously polled request; all byte fields are empty. */
} SWVRequest;
typedef struct {
    uint64_t request_id;
    uint32_t status_code; /* One HTTP hop only. Reqwest redirect following MUST be disabled. */
    SWVBytes final_url; SWVBytes status_text;
    const SWVHeader* headers; size_t header_count;
    SWVBytes body;
    int32_t transport_error; /* Nonzero is a real failed request; status/header/body ignored. */
    SWVBytes error_message;
} SWVResponse;

/* Call initialize once on the dedicated owner worker, before any other WTF/JSC
 * initialization. All functions, including completions/destroy/shutdown, MUST
 * execute on that same thread. Network workers clone request spans and return
 * responses to the owner; they never call this ABI directly.
 * All input spans are borrowed only for the call. All returned spans/pixels are
 * view-owned until the next call mutating that view. poll_request spans last
 * until the next poll_request call. Clone them before sending across threads.
 * Host must perform TLS verification, exact method/header/body, HTTP(S) only,
 * disable redirects and cookie automation, and report failures explicitly.
 * Navigation, redirects, CSP, CORS/CORP and response policy remain WebCore's.
 * Host must cancel its outstanding tasks before destroying their view.
 * Clipboard, media, blob URLs, file uploads, WebSockets, service workers,
 * iframe creation, popup windows, local/session storage, downloads, IME composition, undo/redo and
 * OS dialogs are not available in this initial ABI. Editing text and DOM
 * SWV_KEY_DOWN sends RawKeyDown. If handled=1, suppress its matching Char;
 * WebCore does not synthesize a Char from that call. SWV_KEY_CHAR may carry
 * committed Unicode text. This keeps DOM preventDefault and Tab traversal.
 * Keyboard/mouse/wheel events are native; a handled=0 result means WebCore
 * dispatched the event without consuming its default action.
 */
int32_t scarlet_webview_initialize(SWVBytes font_configuration_path);
int32_t scarlet_webview_shutdown(void); /* Requires all views destroyed. Cannot reinitialize. */
/* Sole thread-safe exception: may run on another thread to interrupt currently
 * executing JS for application shutdown. It uses the already-published native
 * VM termination trap, never creates/accesses WebCore objects on that thread.
 * Host must signal its worker mailbox too, then JOIN that worker. Native view
 * destruction/shutdown run on the owner before host C++ global finalization. */
int32_t scarlet_webview_request_termination(void);
int32_t scarlet_webview_create(const SWVConfiguration*, ScarletWebView** out_view);
int32_t scarlet_webview_destroy(ScarletWebView*);
int32_t scarlet_webview_navigate(ScarletWebView*, SWVBytes absolute_url);
int32_t scarlet_webview_reload(ScarletWebView*);
int32_t scarlet_webview_back(ScarletWebView*);
int32_t scarlet_webview_forward(ScarletWebView*);
/* Owner-only immutable surface limit, combining embedding and actual GPU limits.
 * Hosts can lower backing scale while retaining CSS viewport/input coordinates. */
int32_t scarlet_webview_maximum_surface_dimension(ScarletWebView*, uint32_t* out);
int32_t scarlet_webview_resize(ScarletWebView*, uint32_t physical_width, uint32_t physical_height, double device_scale_factor);
int32_t scarlet_webview_focus(ScarletWebView*, uint32_t focused);
int32_t scarlet_webview_tick(ScarletWebView*); /* Nonblocking native RunLoop cycle + layout/CPU paint. */
/* Tick advances DOM/rendering lifecycle; pixel acquisition rasterizes pending
 * damage once, when the host is ready to consume a frame. */
int32_t scarlet_webview_needs_paint(ScarletWebView*, uint32_t*);
int32_t scarlet_webview_pixels(ScarletWebView*, SWVPixels*);
/* Owned native BGRA8 capability, no CPU framebuffer/readback. At most two
 * leases are live. Release on the owner when its consumer stops sampling,
 * or immediately when discarding a never-presented frame. Tokens remain
 * valid across navigation and viewport changes. */
typedef struct { int32_t handle; uint32_t width, height, format; uint64_t generation; } SWVGpuFrame;
// Output handles own only a synchronized mailbox. These three operations are
// callable from any thread, including after the Page has been destroyed.
typedef struct SWVCompositorOutput SWVCompositorOutput;
int32_t scarlet_webview_compositor_output(ScarletWebView*, SWVCompositorOutput**);
void scarlet_webview_output_destroy(SWVCompositorOutput*);
int32_t scarlet_webview_output_acquire(SWVCompositorOutput*, SWVGpuFrame*, uint64_t* image, uint64_t* epoch, double* scale);
/* Physical bounding damage accumulated across completions skipped before acquire.
 * Query a leased generation on any thread; first/resize/navigation output is full. */
int32_t scarlet_webview_output_damage(SWVCompositorOutput*, uint64_t generation, uint32_t rect[4]);
int32_t scarlet_webview_output_release(SWVCompositorOutput*, uint64_t generation);
// Owner-only: tag subsequent scene commits for navigation filtering.
int32_t scarlet_webview_set_output_epoch(ScarletWebView*, uint64_t epoch);
int32_t scarlet_webview_commit(ScarletWebView*);
/* Owner-thread observation: 0 idle, 1 queued/submitted/pending, 2 ready.
 * The compositor advances GPU completion independently. This query does not
 * run callbacks, tick, or paint. Unsupported profiles return SWV_UNSUPPORTED. */
int32_t scarlet_webview_gpu_frame_state(ScarletWebView*, uint32_t*);
int32_t scarlet_webview_gpu_frame_capacity(ScarletWebView*, uint32_t*);
int32_t scarlet_webview_gpu_frame(ScarletWebView*, SWVGpuFrame*);
/* Identifies the allocation of a currently leased GPU frame. Stable across
 * paints/returns on that allocation; fresh after reallocation. IDs are scoped
 * to one view. A returned or unknown generation is SWV_STALE_REQUEST. */
int32_t scarlet_webview_gpu_frame_image_id(ScarletWebView*, uint64_t generation, uint64_t* out_image_id);
int32_t scarlet_webview_release_gpu_frame(ScarletWebView*, uint64_t generation);
/* Diagnostic readback of one leased image for renderer contract tests.
 * Browser presentation never calls this. The returned pixels are view-owned. */
int32_t scarlet_webview_gpu_frame_readback(ScarletWebView*, uint64_t generation, SWVPixels*);
int32_t scarlet_webview_state(ScarletWebView*, SWVState*);
int32_t scarlet_webview_mouse(ScarletWebView*, const SWVMouseEvent*, uint32_t* handled);
int32_t scarlet_webview_wheel(ScarletWebView*, const SWVWheelEvent*, uint32_t* handled);
int32_t scarlet_webview_key(ScarletWebView*, const SWVKeyEvent*, uint32_t* handled);
int32_t scarlet_webview_poll_request(ScarletWebView*, SWVRequest*); /* SWV_EMPTY: no pending request. */
int32_t scarlet_webview_complete_request(ScarletWebView*, const SWVResponse*);
/* Incremental alternative: start headers, append decoded entity bytes, then
 * finish exactly once. Policy checks precede delivery. Cancellation invalidates
 * the ID. Owner-thread calls only; each span is copied before return.
 * complete_request with transport_error also fails an in-progress stream. */
int32_t scarlet_webview_start_response(ScarletWebView*, const SWVResponse*);
int32_t scarlet_webview_append_response(ScarletWebView*, uint64_t request_id, SWVBytes, uint32_t finished);
/* Read-only actual DOM text inspection for embedding smoke tests. */
int32_t scarlet_webview_text_content(ScarletWebView*, SWVBytes element_id, SWVBytes*);
/* Actual raster work counters, for embedding performance verification. */
typedef struct SWVPaintStatistics { uint64_t paints; uint64_t painted_pixels; } SWVPaintStatistics;
int32_t scarlet_webview_paint_statistics(ScarletWebView*, SWVPaintStatistics*);
/* Separate retained-backing raster work from final page-image compositions.
 * tile_pixels counts actual clipped device-pixel tile raster work, not CSS area.
 * OFF builds zero the output and return SWV_UNSUPPORTED. */
typedef struct SWVCompositingStatistics {
    uint64_t tile_paints, tile_pixels;
    uint32_t enabled, reserved;
} SWVCompositingStatistics;
int32_t scarlet_webview_compositing_statistics(ScarletWebView*, SWVCompositingStatistics*);

/* Read-only video inspection on the view owner. Enumerates actual HTML video
 * elements in the main Document, excluding subframes and shadow trees.
 * Natural dimensions are video pixels; rectangles are CSS viewport units.
 * Duration retains DOM NaN/infinity for unknown or live media. Counters are
 * actual MediaPlayer decoder/drop and compositor-submission statistics. */
typedef struct ScarletWebViewMediaStatistics {
    uint32_t video_count, width, height, paused, ended, ready_state;
    uint64_t decoded_frames, dropped_frames, presented_frames;
    double current_time, duration, rect_x, rect_y, rect_width, rect_height;
} ScarletWebViewMediaStatistics;
/* SWV_EMPTY: no video element; SWV_INVALID_ARGUMENT: index outside a nonempty
 * collection. video_count remains populated in either case. */
int32_t scarlet_webview_media_statistics(ScarletWebView*, uint32_t index, ScarletWebViewMediaStatistics*);

#ifdef __cplusplus
}
#endif
#endif
