# Native persistent C ABI smoke

`ScarletWebViewSmoke.cpp` is an integration executable for the configured native
Scarlet WebCore embedding. It includes only the public C ABI and POSIX/native
runtime interfaces. Its HTML, CSS, SVG and JavaScript fixtures are original test
inputs. The executable supplies deterministic HTTP response bytes through the
same `poll_request` / `complete_request` boundary used by the browser transport;
it does not connect to a server or claim TLS/network verification. WebCore must
construct and process every request, document, security decision and frame.

Build this source as `scarlet-webview-smoke`, with C++23 and the native Scarlet
toolchain. Link it against `ScarletWebView` and its complete generated native
closure, including the SDK's C++ support and final-executable C startup adapter.
Use the same static archive grouping and final link options as the embedding
manifest. The source adds an ordinary `main`; it needs no WebCore private
headers, host libraries, special abort handler or alternate process termination.
The native runtime must provide POSIX threads, an 8 MiB pthread stack,
`clock_gettime(CLOCK_MONOTONIC)`, `nanosleep`, and normal C++/TLS finalization.

Run two separate native guest processes, with an outer timeout and child exit
status capture:

```sh
scarlet-webview-smoke --run-id UNIQUE_BASIC --fonts /etc/fonts/fonts.conf --mode basic
scarlet-webview-smoke --run-id UNIQUE_TERMINATION --fonts /etc/fonts/fonts.conf --mode termination
```

Each run ID must contain 1–96 ASCII letters, digits, underscores or hyphens. The
default font configuration is explicitly `/etc/fonts/fonts.conf`; its actual
native font files must be installed in the guest. All diagnostic records start
with `SCARLET_WEBVIEW_SMOKE ` followed by JSON. Numeric diagnostics use integer
formats because the native libc's `printf` does not implement floating point.
An outer verifier must require the matching unique run ID, a final `result` with
`pass:true`, `owner_join_status:0`, and the actual process exit status 0. A printed
pass before CRT/TLS failure is insufficient. Do not count source inspection or a
host syntax check as native runtime evidence.

The basic mode owns one WebView on a real native pthread and retains it across:

- HTML, external CSS/JavaScript/SVG and asynchronous fetch response delivery;
  matching CSS/SVG pixel samples and actual DOM text prove these were consumed.
- Allowed and rejected CORS fetches with WebCore's exported Origin/credential
  metadata, plus rejected cross-origin CORP image and
  stylesheet responses. The rejected stylesheet would visibly change the
  sampled pixels if WebCore incorrectly accepted it.
- Native mouse click and keyboard `preventDefault`, ordinary editable text and
  committed Unicode, with real event handlers mutating the DOM and framebuffer.
  A separate real textarea receives a commit beginning with a newline and
  containing Japanese, a non-BMP strawberry character and accented Latin text,
  followed by a distinct U+F700 private-use commit. An actual `input` handler
  copies `textarea.value` into DOM text after each commit; reading a textarea's
  initial DOM text alone is not evidence that editing occurred.
- Physical resize, resize events, device scale factor 2, unchanged pixels and
  generation during 64 idle ticks, native wheel scrolling and focus changes.
- New document navigation, real back/forward history, reload with a new polled
  request, and a redirect delivered one HTTP hop at a time.
- Attachment response rejection before its forbidden HTML/script commits,
  injected transport failure, and rejection of a duplicate completion ID.
- Wrong-thread initialization/state rejection; owner-thread destroy/shutdown;
  actual owner join before returning through ordinary CRT/TLS finalization.

The route-count diagnostic uses this fixed order: first HTML, CSS, external JS,
SVG, same-origin fetch, allowed CORS fetch, denied CORS fetch, denied CORP CSS,
denied CORP SVG, second HTML, redirect, attachment, transport failure, infinite
script. Cached history restoration may avoid additional first-document requests;
reload and the redirect destination explicitly require new second-document
requests. No cookie automation or cross-thread ABI completion is used.

The termination mode initializes a fresh process/VM. Its document executes an
infinite JavaScript loop whose body sets an actual DOM marker. After the
response is armed, the main controller thread calls only the documented
thread-safe `request_termination` API. Success requires a real native script
entry, the marker proving that the loop body executed, return to the owner,
owner destroy/shutdown and an actual
join. The test never detaches the owner or reports a bounded join as success.
Failure to interrupt remains visible as the outer guest timeout. This separate
process intentionally tests application shutdown, not reuse of a terminated VM.
