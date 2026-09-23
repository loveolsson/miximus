# CEF media inputs and virtual webcams: exploration

Initial research: **2026-09-22**. Custom-build reassessment: **2026-09-23**.

This document records a separate exploration of feeding Miximus video **into** a web page hosted by CEF. It is not
part of the [main CEF browser-source plan](cef-browser-sources.md), an implementation commitment, or an additional
prerequisite for browser-source support. Miximus now patches and builds CEF/Chromium for accelerated browser output.
No media-input prototype, runtime injection, or media-input platform benchmark has been performed. The reassessment
below uses the pinned local source checkout; it does not authorize media-input implementation or change the main plan.

## Purpose and constraints

Broadcast templates often need live video inside their HTML composition. Today that commonly means opening a real
capture device or installing a virtual webcam. The desired Miximus behavior is to connect ordinary graph textures to
inputs on the CEF node and expose those inputs to its page as real browser `MediaStream`s.

The constraints established during discussion are:

- Do not require OS-enumerated virtual webcams or drivers.
- Keep frame delivery from Miximus into Chromium on the GPU, including any bridge-owned staging or copying before
  delivery to the browser track. GPU pixel-format conversion and GPU copies are allowed; CPU pixel readback/upload
  and transcoding are not part of this ingress path. CPU handling of metadata, handles and completion is allowed.
- Chromium's subsequent handling of an ingested frame is outside that GPU-only restriction. Its internal copies,
  consumer-requested readbacks and one-time pause copy are acceptable and do not require replacement implementations.
- The stock CPU delivery path below is retained as historical research. It is not the selected direction or an
  authorized fallback for GPU ingress.
- Do not use video encoding/decoding or compressed transport; pixel-format and color conversion are allowed.
- Keep streams associated with node input slots through disconnection, reconnection and source-format changes.
- Defer buffering depth, frame-selection policy and exact timing behavior.
- Contain the feature in the CEF node, node details and app-owned subsystem. Upstream and downstream nodes use ordinary
  texture interfaces and do not know about browser transport.
- Initially consider documented stock-CEF APIs. Separately explore Chromium internals and binary injection to understand
  the actual boundary; that exploration does not authorize adopting those techniques in the main plan.

Here, “media source” usually means a producer behind a `MediaStreamTrack`. The JavaScript `MediaSource` API for Media
Source Extensions is a different playback mechanism; it is not the native GPU-frame injection interface sought here.

## Evidence and confidence

The source investigation uses **CEF `152.0.8+g1ce985c+chromium-152.0.7977.134`**, at
[`1ce985cb23056548b9cc51483bbef4faf68b1cd3`](https://github.com/chromiumembedded/cef/tree/1ce985cb23056548b9cc51483bbef4faf68b1cd3),
and the matching [Chromium `152.0.7977.134` tag](https://github.com/chromium/chromium/tree/152.0.7977.134).
These are the baseline selected for the main investigation, not a floating dependency recommendation.

| Finding | Evidence level |
| --- | --- |
| CEF has media permission callbacks, shared-memory process messages and native JavaScript bindings | Public headers inspected |
| No public CEF API was found for custom capture-device registration or external GPU-backed video-frame creation | Public SDK/header inventory and relevant implementation inspected |
| Chromium can import native GPU resources and wrap them as media frames | Platform importers and native frame APIs inspected |
| Chromium has an internal texture-fed virtual camera | Service definitions, adapter, renderer path and browser test inspected |
| A native bridge could feed a GPU-backed frame into a page track | Engineering inference from existing import, frame and Blink source APIs, revisited in the local checkout |
| Miximus can build and distribute a patched runtime | Existing accelerated-output implementation; not evidence that media input works |
| Patch-size estimates for media input | Source-based engineering estimates; no prototype or measured implementation effort |
| An exact-binary shim might reach those internals without rebuilding CEF | Plausible private object layout and entry points identified; not demonstrated |
| Any proposed path meets Miximus performance and portability requirements | Not established; runtime qualification required |

“Stock CEF has no public entry point” must not be shortened to “Chromium cannot do GPU media input” or “CPU copies are
unavoidable.” Likewise, a custom build is one way to expose the functionality, not a strict technical necessity if
binary-specific integration is permitted.

## Desired page-facing behavior

The user's proposed hook is:

```js
const stream = await window.miximus.getInputMediaStream({ inputIndex: 0 });
video.srcObject = stream;
```

An asynchronous return is suggested because setup crosses process boundaries. The precise options, subscription
ownership and repeated-call behavior remain undecided. A valid but disconnected input should still yield a real
stream with a live video track; resolving the request should not wait for the first connected source frame.

| Event | Desired behavior |
| --- | --- |
| Valid slot initially disconnected | Return the stream; placeholder frames are a proposed way to initialize video playback |
| Source connected | Deliver source frames through the same stream and track |
| Source replaced or disconnected | Keep the track alive; change its producer or resume placeholder delivery |
| Dimensions change | Accept new frame dimensions without requiring the page to reacquire the stream |
| Native texture format changes | Adapt conversion/pools internally; expose a supported browser frame representation |
| Navigation, session destruction or subscription stop | Release the appropriate subscription and resources |

Placeholder appearance, initial size and cadence are open choices, not agreed defaults. Exact resize behavior and
consumer reactions need testing against the selected runtime. Fixed-size constraints could intentionally prevent
following the source dimensions and should not be imposed implicitly.

Generated tracks do not automatically appear in `enumerateDevices()` or become selectable through `getUserMedia()`.
This hook targets cooperating templates. An opt-in JavaScript compatibility adapter for existing webcam-oriented
templates is conceivable, but emulating device IDs, enumeration, constraints, device-change events and permission
behavior is a separate project. It is not equivalent to registering a native device.

## Stock CEF: supported uncompressed delivery (historical alternative)

This section describes a technically available CPU route from the initial investigation. It does not satisfy the
current GPU-only ingress requirement and is not proposed for implementation.

CEF's [permission handler](https://github.com/chromiumembedded/cef/blob/1ce985cb23056548b9cc51483bbef4faf68b1cd3/include/cef_permission_handler.h)
can approve or deny a page's camera/microphone request. It does not accept a custom device or pixel producer.
[CefMediaRouter](https://github.com/chromiumembedded/cef/blob/1ce985cb23056548b9cc51483bbef4faf68b1cd3/include/cef_media_router.h)
discovers Cast/DIAL devices; its media-source objects are unrelated to injecting local video frames.

The documented route is to deliver raw pixels to the renderer and create frames for a generated track:

```mermaid
flowchart LR
    T[Ordinary input texture] --> R[Conversion and bounded readback]
    R --> M[CEF shared-memory message]
    M --> B[Renderer-owned pixel buffer]
    B --> F[VideoFrame]
    F --> G[Generated video track]
    G --> S[MediaStream in page]
```

Chromium 152 exposes `MediaStreamTrackGenerator` on `Window`. The newer `VideoTrackGenerator` is still test-gated in
this revision; the current W3C draft must not be mistaken for the installed runtime's API. Use the available generator
behind the page adapter rather than enabling experimental flags. Preparation can run off-thread where supported,
but do not assume that the generator itself can be constructed in a worker. See
[Chrome's documented API](https://developer.chrome.com/docs/capabilities/web-apis/mediastreamtrack-insertable-media-processing),
the [pinned generator IDL](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/modules/breakout_box/media_stream_track_generator.idl),
and [feature gates](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/platform/runtime_enabled_features.json5#L6820).

`VideoFrame` can be constructed from raw bytes, dimensions, layout, color information and a timestamp. This uses a
WebCodecs raw-frame type but performs **no encoding or decoding**. The track can be consumed by a video element and
other normal browser media consumers. Creating a generated track from already supplied data does not require opening
a camera. See [WebCodecs raw frames](https://www.w3.org/TR/webcodecs/#videoframe-interface).

### Shared memory and JavaScript ownership

[CefSharedProcessMessageBuilder](https://github.com/chromiumembedded/cef/blob/1ce985cb23056548b9cc51483bbef4faf68b1cd3/include/cef_shared_process_message_builder.h)
provides native browser/renderer shared-memory transport. The builder is invalidated by `Build()`; the receiver obtains
a mapping through `GetSharedMemoryRegion()`. This is a building block, not a ready-made reusable video ring. Application
flow control must bound outstanding payloads and retain mappings through use. Do not send pixels through JSON, base64,
the control-message router, or the editor WebSocket protocol.

CEF 152's `CefV8BackingStore` supports allocating storage on a valid V8 thread, filling it from a background thread,
and consuming it to create a JavaScript `ArrayBuffer` without a copy at that final handoff. It does not directly wrap
the incoming shared-memory mapping. The older arbitrary-memory `CreateArrayBuffer()` explicitly returns null when
the V8 sandbox is enabled. Budget for copying into V8-owned storage; do not disable the sandbox to remove that copy.
See the [selected V8 APIs](https://github.com/chromiumembedded/cef/blob/1ce985cb23056548b9cc51483bbef4faf68b1cd3/include/cef_v8.h#L429).

Buffer transfer into `VideoFrame` can avoid an additional copy where supported. It does not eliminate GPU readback,
earlier copies or browser upload. Once ownership is handed to Chromium, a successful write/queue acknowledgement is
not permission to overwrite its storage. Bound the application's outstanding work, close frames promptly, and handle
a page that stalls or retains frames; do not assume JavaScript garbage collection provides timely slot reuse.

### Expected costs

Uncompressed BGRA payload alone, calculated as width × height × four bytes × frame rate:

| Frame size and rate | Payload per second |
| --- | --- |
| 1920×1080 at 60 fps | 497.7 MB/s |
| 3840×2160 at 60 fps | 1.99 GB/s |

These are decimal payload rates, not measured bus traffic. Readback, CPU copies and browser upload add costs.
YUV conversion can reduce payload for opaque video but changes chroma/quality and format requirements. Alpha, range,
transfer function and orientation need explicit treatment. The initial acceptance of CPU delivery did not establish
unlimited multi-UHD capacity; that route is now outside the selected ingress requirements.

CEF-output-to-Vulkan sharing is the opposite direction and does not solve this input path. Similarly, WebGPU's
`importExternalTexture()` accepts an existing browser video element or `VideoFrame`, not an arbitrary Vulkan/OS handle.
See [WebGPU external textures](https://www.w3.org/TR/2026/CRD-webgpu-20260512/#external-textures).

## Do virtual cameras always copy through the CPU?

No universal CPU-copy requirement was found. The Windows [VCamSample implementation](https://github.com/smourier/VCamSample#notes)
can produce GPU-backed samples when the consumer supplies a Direct3D manager and uses a CPU path otherwise. This is
evidence of a GPU-capable source, not a guarantee that every application consuming it avoids readback.
Apple's [IOSurface framework](https://developer.apple.com/documentation/iosurface) also supports framebuffer sharing
across processes; [Core Video documents](https://developer.apple.com/documentation/corevideo/cvpixelbuffercreatewithiosurface%28_%3A_%3A_%3A_%3A%29)
the need to prevent pool reuse while other processes still use the backing surface.

More directly, Chromium's selected Windows capture implementation extracts an `ID3D11Texture2D` from Media Foundation
samples when hardware capture is available and source/requested formats are NV12. `DeliverTextureToClient()` either:

- forwards a suitable external shared texture when the corresponding zero-copy feature is enabled; or
- copies into a GPU-memory-buffer texture with `CopySubresourceRegion()`.

The copy path waits for GPU completion before handing the resource across the device/process boundary. That CPU wait
does not copy pixels through CPU memory. Software consumers, premapping requests and fallback conditions can still
cause CPU access. A compatible virtual camera can use the same native capture route because it supplies the Media
Foundation contract Chromium already consumes. See
[Windows capture](https://github.com/chromium/chromium/blob/152.0.7977.134/media/capture/video/win/video_capture_device_mf_win.cc#L2230)
and its [GPU copy/completion logic](https://github.com/chromium/chromium/blob/152.0.7977.134/media/capture/video/win/video_capture_device_mf_win.cc#L685).

These findings corrected the assumption that all virtual cameras require CPU copies. They do not change the current
Miximus requirement to deliver input frames into Chromium on the GPU.

## The native GPU boundary inside Chromium

The required chain already has implementations in Chromium:

```mermaid
flowchart LR
    V[Miximus Vulkan texture] --> P[Compatible exported platform resource]
    P --> I[Chromium GPU service: SharedImage import]
    I --> F[media::VideoFrame]
    F --> B[Blink video source or VideoFrame wrapper]
    B --> S[Page MediaStream]
```

### Importing external resources

`SharedImageInterface::CreateSharedImage()` has an overload accepting `gfx::GpuMemoryBufferHandle`. A `SharedImage`
adds Chromium-managed identity, format, usage and synchronization around the resource; it is not just a native handle.

| Platform | Existing importer | Remaining Miximus questions |
| --- | --- | --- |
| Windows | DXGI handle opened as a D3D11 texture | Compatible export allocation, matching adapter, format/usage and cross-API synchronization |
| macOS | IOSurface-backed shared image | IOSurface/Metal/MoltenVK allocation and synchronization compatibility |
| Linux/Ozone | Native pixmap/DMA-BUF-backed shared image | Plane layout, DRM modifiers, driver/backend support and explicit producer/consumer ordering |

An arbitrary `VkImage` is insufficient. A compatible shared pool or GPU conversion/copy into one may be necessary.
The Windows importer validates size and format and ultimately opens the resource with `OpenSharedResource1()`.
See [shared-image creation](https://github.com/chromium/chromium/blob/152.0.7977.134/gpu/command_buffer/client/shared_image_interface.h#L149),
[Windows](https://github.com/chromium/chromium/blob/152.0.7977.134/gpu/command_buffer/service/shared_image/d3d_image_backing_factory.cc#L829),
[macOS](https://github.com/chromium/chromium/blob/152.0.7977.134/gpu/command_buffer/service/shared_image/iosurface_image_backing_factory.mm#L411),
and [Linux](https://github.com/chromium/chromium/blob/152.0.7977.134/gpu/command_buffer/service/shared_image/ozone_image_backing_factory.cc#L194).

CEF does not expose the required GPU-channel/shared-image interface or an equivalent external-frame registration API.
That is the first missing application-facing connection.

### Native frame and track creation

`media::VideoFrame::WrapSharedImage()` accepts a shared image, synchronization token, release callback, geometry and
timestamp. It does not inherently require a CPU pixel buffer. Blink's `PushableMediaStreamVideoSource::PushFrame()`
accepts these native frames; its broker handles thread handoffs. `MediaStreamTrackGenerator` uses that source machinery.

Alternatively, Blink's native `VideoFrame` constructor can wrap a `media::VideoFrame` as a real JavaScript video frame,
which could then be fed to the existing JavaScript generator. Stock CEF's V8 value API has no corresponding GPU-frame
factory. These are the second and third missing connections, rather than missing rendering capabilities.
See [native frame wrapping](https://github.com/chromium/chromium/blob/152.0.7977.134/media/base/video_frame.h#L212),
[pushable source](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/modules/breakout_box/pushable_media_stream_video_source.h),
and [Blink frame wrapper](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/modules/webcodecs/video_frame.h#L58).

### Internal virtual camera

Chromium's `VideoSourceProvider` offers `AddSharedMemoryVirtualDevice()` and `AddTextureVirtualDevice()`. These devices
exist within Chromium's capture service and need no OS webcam registration. The texture interface registers exported
shared images, publishes frames by buffer ID and uses `OnFinishedConsumingBuffer()` for its consumption protocol.

A browser test creates GPU-backed images, registers an internal camera, alternates its frames and tracks consumption.
The renderer imports the shared image and wraps it as a native video frame. This proves an engine-level route exists;
the test does not validate live writes by an external Vulkan producer. These Chromium service definitions are not an
exported stock-CEF extension API, despite residing in Chromium directories named `public`.
See [provider](https://github.com/chromium/chromium/blob/152.0.7977.134/services/video_capture/public/mojom/video_source_provider.mojom),
[virtual-device contract](https://github.com/chromium/chromium/blob/152.0.7977.134/services/video_capture/public/mojom/virtual_device.mojom),
[browser test](https://github.com/chromium/chromium/blob/152.0.7977.134/content/browser/webrtc/webrtc_video_capture_service_browsertest.cc#L96),
and [renderer wrapping](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/platform/video_capture/video_capture_impl.cc#L373).

### Two custom-runtime integration candidates

1. Expose the internal virtual-camera service and shared-image import to the application. Existing capture machinery
   then creates the page's track, but camera enumeration, selection and permission semantics accompany it.
2. Expose imported frames directly to a per-page pushable source or JavaScript `VideoFrame`. This better matches the
   proposed persistent input-slot hook and avoids introducing camera semantics.

The second is the better conceptual fit for Miximus; the first has a useful existing end-to-end browser test. Neither
has been implemented here. Source inspection does not establish whether a direct bridge can live entirely in CEF or
needs a Blink-side adapter. No compressed media transport is needed by either route.

### Synchronization is part of the bridge

- Transfer actual platform resource ownership across processes; a numeric handle in JSON is not sufficient.
- Connect Miximus producer completion to Chromium readiness. A Chromium `SyncToken` is not a Vulkan semaphore.
- Do not reuse storage until consumer GPU access has retired. A frame-destruction or consumption notification must be
  interpreted together with the relevant release synchronization, not assumed to be a universal GPU fence.
- Track format, color, adapter, pool generation, resize, navigation, device loss and shutdown explicitly.
- Distinguish no CPU pixel round trip from literal zero-copy: GPU conversion/copy may remain necessary, and a page's
  chosen consumers can request software access.

The shared-image interface includes external updates with an acquire fence, showing that external producers are an
anticipated use case. Platform/backend applicability still requires validation. See
[updates and fences](https://github.com/chromium/chromium/blob/152.0.7977.134/gpu/command_buffer/client/shared_image_interface.h#L180).

## Reassessment with the existing custom CEF build

The build and packaging obstacle has changed: Miximus already maintains a pinned, patched CEF/Chromium runtime for
accelerated browser output. A source-level media-input bridge is therefore a reasonable candidate for a focused
prototype. It would be larger than the existing capture fixes, but should primarily connect existing implementations
rather than introduce a new media pipeline. Existing output qualification does not establish input-path correctness.

### Preferred integration boundary

Prefer the explicit per-page `getInputMediaStream({ inputIndex })` model over internal camera enumeration. Preserve
stream/track identity through source changes, and create the stream independently of whether the slot is connected.
Placeholder appearance, buffering and cadence remain undecided. The page-facing Miximus hook belongs in our renderer
integration; the runtime extension can expose narrower native frame/stream operations without globally changing the
browser's JavaScript APIs.

The custom runtime would need three parts:

| Part | Required work |
| --- | --- |
| CEF-facing interface | Register external texture buffers, submit frame metadata, receive release/error notifications, and associate subscriptions with an authorized browser context |
| Native transport and GPU ownership | Transfer actual platform handles, import images, establish producer readiness, bound outstanding resources, and retire buffers safely |
| Renderer/Blink adapter | Connect imported `media::VideoFrame`s to a persistent track and expose the resulting object to JavaScript |

CEF's existing frame IPC carries ordinary values and shared-memory regions. Extend it or add a dedicated Mojo
interface for native descriptors and lifecycle messages. IPC carries handles, identifiers, timestamps and
acknowledgements; it does not serialize texture pixels. Resource registration can be separate from per-frame
submission so a pool slot need not be imported anew for every frame. See
[CEF frame transport](https://github.com/chromiumembedded/cef/blob/1ce985cb23056548b9cc51483bbef4faf68b1cd3/libcef/common/mojom/cef.mojom#L53).

Two renderer adapters are credible:

- Feed native frames directly into a pushable video source. This gives native code more direct control over delivery
  and ownership and avoids routing every frame through page JavaScript.
- Expose a GPU-backed JavaScript `VideoFrame` and feed the existing JavaScript track generator. This may yield the
  smallest initial demonstration; constructing this raw-frame object does not encode or decode video.

The exact adapter remains a prototype decision. Budget for a small Blink-facing adapter rather than asserting that
all code can live solely in CEF: native stream/source interfaces exist, but JavaScript wrapping, execution-context
lifetime and build dependencies still cross Blink's boundary. Existing `MediaStreamTrackGenerator` constructs the
pushable source, and Blink's native `VideoFrame` constructor accepts a `media::VideoFrame`. Neither approach should
require rewriting the compositor, implementing codecs, or modifying OS camera drivers. See
[generator construction](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/modules/breakout_box/media_stream_track_generator.cc),
[embedder source interface](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/public/web/modules/mediastream/media_stream_video_source.h),
and [native JavaScript-frame wrapper](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/modules/webcodecs/video_frame.h#L60).

### Safe reuse is the main engineering boundary

The internal virtual-camera browser test alternates between two textures whose contents do not change after creation.
In this revision, the renderer's `kSharedImageHandle` branch wraps the image with an empty `ReleaseMailboxCB`. These
facts establish a GPU transport/playback route, but not a complete synchronization contract for external Vulkan
textures rewritten each frame. They are not evidence that the upstream camera implementation is generally broken.
See [the static-texture fixture](https://github.com/chromium/chromium/blob/152.0.7977.134/content/browser/webrtc/webrtc_video_capture_service_browsertest.cc#L96)
and [the renderer branch](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/platform/video_capture/video_capture_impl.cc#L373).

For the first prototype, consider a GPU copy into Chromium-owned frame storage:

```mermaid
flowchart LR
    T[Ordinary Miximus input texture] --> E[GPU conversion/copy into bounded export pool]
    E --> I[Chromium imports native handle]
    I --> C[GPU copy into Chromium-owned frame storage]
    C --> F[Native VideoFrame]
    F --> S[Persistent MediaStreamTrack]
```

This adds a GPU copy, but creates a simpler ownership boundary: Miximus can reuse its transfer slot after Chromium's
copy has completed on the GPU, independently of how long the page retains the resulting frame. The bridge must also
bound its Chromium-owned frame storage; allocating a new destination indefinitely would merely move the pressure to
another process. A stalled or retaining consumer must exhaust bounded admission rather than stall graph rendering or
cause unbounded allocation.

Chromium already provides `RasterInterface::CopySharedImage()`. `SharedImageInterface::UpdateSharedImage()` accepts
an acquire fence, and the Ozone backing stores an external-write fence. Those are useful building blocks, not proof
that every chosen driver, format and backend combination works. Completion must be established for the actual GPU
copy: a message acknowledgement, frame destructor or signalled command-stream token must not automatically be treated
as an external Vulkan completion fence. Host-side notification or waiting can coordinate completion without moving
pixel data through CPU memory. See
[GPU image copying](https://github.com/chromium/chromium/blob/152.0.7977.134/gpu/command_buffer/client/raster_interface.h#L60),
[external acquire fences](https://github.com/chromium/chromium/blob/152.0.7977.134/gpu/command_buffer/client/shared_image_interface.h#L180),
and [Ozone external updates](https://github.com/chromium/chromium/blob/152.0.7977.134/gpu/command_buffer/service/shared_image/ozone_image_backing.cc#L119).

A direct shared-allocation path could avoid the additional Chromium-side copy, but would couple external storage reuse
to browser-consumer lifetimes and their GPU release synchronization. Investigate that optimization after establishing
the copy-based contract; it is not a prerequisite. The GPU-only requirement ends at delivery into Chromium's media
path. It does not impose a GPU-only implementation on every subsequent browser operation or consumer.

### Pause behavior is acceptable

The intended use is live camera sources inside HTML graphics, where pause is uncommon. In the pinned Chromium
revision, pausing a `MediaStream` video schedules `ReplaceCurrentFrameWithACopy()`. For a GPU-backed current frame,
`CopyFrame()` allocates a CPU bitmap and wraps its pixels in a CPU-backed `VideoFrame`. This retains a frozen image
while releasing the original producer buffer. It is a copy of the current frame into RAM, not a disk-cache operation
or continuous copying of incoming frames while paused. This behavior is acceptable and requires no additional patch.
See [pause handling](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/modules/mediastream/web_media_player_ms.cc#L867),
[paused-frame copying](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/modules/mediastream/web_media_player_ms_compositor.cc#L66),
and [CPU-frame wrapping](https://github.com/chromium/chromium/blob/152.0.7977.134/media/base/video_util.cc#L891).

Pausing one `<video>` element does not necessarily stop its underlying track or other consumers. Suspending ingress
when there are no active consumers is a possible optimization, subject to verifying the relevant feedback; it is not
required to accommodate Chromium's pause copy. Retain normal bounded-resource and completion rules in either case.

### Patch size and maintenance estimate

These are rough engineering estimates from source inspection, not prototype measurements or delivery commitments.

| Scope | Estimated magnitude |
| --- | --- |
| Linux proof of concept: one input, one format, basic frame delivery | Hundreds to low thousands of handwritten lines |
| Maintainable runtime bridge: bounded resources, context lifecycle, resize, cancellation and GPU-process recovery | Approximately **2,000–5,000 handwritten lines**, excluding generated CEF bindings and tests |
| Blink-specific portion of that bridge | Likely hundreds of lines for an adapter, rather than changes throughout the media implementation |
| Miximus node/subsystem integration and tests | Additional work, outside the runtime estimate |
| Windows/macOS qualification | Additional platform-specific allocation, handle-transfer and synchronization work; Linux success does not establish portability |

Initially expect to reuse Chromium's existing GPU importers. Compatibility of exported allocations, formats and
synchronization is the largest uncertainty and could require further targeted backend patches beyond the bridge
estimate. The proposed Vulkan-to-CEF direction has not been qualified by the opposite-direction output work.

Keep handwritten changes additive and concentrated in a CEF API/implementation, a dedicated transport interface and a
small renderer/Blink adapter. CEF API changes also require the corresponding generated C/C++ wrappers and versioning
work; generated line counts should not be confused with handwritten complexity. Chromium upgrades may change shared
image ownership, synchronization APIs or Blink lifetime rules, so rebasing must include runtime tests, not just a clean
compile. The direct approach avoids tying the feature to camera enumeration and permission internals, but does not
eliminate Chromium maintenance.

The revised recommendation is to investigate this source-level bridge rather than binary injection. The existing
build pipeline removes a major practical obstacle. Import, synchronization, bounds and crash recovery remain the
qualification work; GPU-backed media representation itself already exists.

## DLLs, private objects and binary-specific injection

This section intentionally goes beyond public CEF APIs to record what may be technically possible. It is not an
implementation recommendation or evidence of a working shim against the selected binary distribution.

A library linked into Miximus or its renderer helper can implement ordinary CEF native JavaScript bindings. Merely
loading that library does not expose Chromium's capture service or GPU objects. A custom runtime could export a small
bridge for it; runtime injection is a different possible way of reaching those objects without changing files on disk.

### Recovering an internal object pointer

CEF's application C++ API wraps its exported C API. The library-side wrapper also retains the actual C++ object:

```text
Application-side CefBrowser wrapper
  → C API structure
    → library-side wrapper
      → internal browser object
        → Chromium WebContents
```

The private `CefCppToCRefCounted::WrapperStruct` contains `BaseName* object_` together with the C structure. CEF's own
code recovers the enclosing wrapper from that structure. Exact-layout-aware code could potentially recover this
pointer without rebuilding CEF. Inside the library, `CefBrowserHostBase` implements `CefBrowser`/`CefBrowserHost` and
has `GetWebContents()`.

This is a concrete candidate boundary. It does not justify casting the application-side `CefBrowser*` directly to the
internal implementation: the wrappers are different objects. Recovery and reference counting would have to match the
actual API version, ABI and build. See
[wrapper layout](https://github.com/chromiumembedded/cef/blob/1ce985cb23056548b9cc51483bbef4faf68b1cd3/libcef_dll/cpptoc/cpptoc_ref_counted.h#L114)
and [internal browser class](https://github.com/chromiumembedded/cef/blob/1ce985cb23056548b9cc51483bbef4faf68b1cd3/libcef/browser/browser_host_base.h).

### Supplying an application-owned virtual class

C++ virtual dispatch can call implementations in another library. The missing part is where to install the instance:
CEF accepts `CefClient`, but does not ask the application for a Chromium capture-device factory. Creating a subclass
or adding methods cannot make already compiled Chromium code discover and call it.

Possible binary-specific techniques include reaching an internal registration function, intercepting factory creation,
or replacing a stored interface pointer. Each requires correct ABI, ownership, threading and process placement. A
capture factory in a subprocess cannot use a raw pointer to an object allocated in Miximus. Code must run in that
process or communicate through IPC. Linking duplicate Chromium implementation/support code into a shim also cannot
be assumed to preserve the original runtime's allocators, singletons and object identities.

### A narrower candidate: existing service entry points

The browser test reaches the camera service through:

```text
GetVideoCaptureService()
  → ConnectToVideoSourceProvider(...)
    → AddTextureVirtualDevice(...)
```

A browser-process shim that could reach this entry point and construct the matching Mojo connection might register
the existing internal camera without replacing a camera vtable. It would separately need shared-image access; the
test gets that from an internal raster context provider. A small number of entry points does not imply a small amount
of integration code. See the
[service connection in the test](https://github.com/chromium/chromium/blob/152.0.7977.134/content/browser/webrtc/webrtc_video_capture_service_browsertest.cc#L430).

Recovering `WebContents` alone is insufficient for the direct-track approach: the Blink objects live in the renderer
subprocess. The CEF helper and renderer callbacks let application code execute there, but internal frame/track access
would still need its own bridge.

### Pointer access, symbol access and process access are different

| Boundary | What a binary-specific shim would require |
| --- | --- |
| Pointer retained in a CEF wrapper | Exact private layout and lifetime handling |
| Virtual method on a recovered object | Correct object type and ABI; actual virtual dispatch must remain available |
| Exported nonvirtual function | Resolve the symbol and call with matching types |
| Present but hidden function | Locate an address in that exact binary, for example using matching symbols |
| Inlined/eliminated function | May have no standalone callable entry point |
| Object in another process | Existing/new IPC or code executing in that process |

CEF intentionally exports a C API to insulate applications from Chromium's C++ ABI. Its Linux export script exports
`cef_*` and hides other symbols when the applicable build configuration uses it. Windows/macOS exports, debug symbols,
optimization, control-flow protections and loading restrictions must be checked on the exact artifact. No such binary
inspection was performed, so simple `dlsym()`/`GetProcAddress()` access cannot be asserted or universally ruled out.
See [ABI explanation](https://chromiumembedded.github.io/cef/general_usage#c-wrapper),
[export script](https://github.com/chromiumembedded/cef/blob/1ce985cb23056548b9cc51483bbef4faf68b1cd3/libcef_dll/libcef.lst),
and [conditional build use](https://github.com/chromiumembedded/cef/blob/1ce985cb23056548b9cc51483bbef4faf68b1cd3/BUILD.gn#L1976).

The conclusion is **plausible binary-specific integration**, not a discovered general-purpose plugin mechanism.
A custom build gives an explicit place to implement the same bridge; an injected shim exchanges that build burden
for dependence on the exact distributed binary. Neither automatically requires disabling the sandbox.

## Alternatives examined and excluded

These are recorded for completeness, not retained as fallback recommendations after the user's no-transcoding decision.

| Alternative | Finding and disposition |
| --- | --- |
| Native WebRTC sender to the page | Can deliver real received tracks, but adds codec/transport machinery; excluded |
| Native encoding plus WebCodecs decoding | Reduces transported bytes but requires encoding/decoding; excluded |
| Encoded stream through a resource handler/media element | Requires media formats and playback buffering; excluded |
| Canvas drawing plus `captureStream()` | Can create a stream without codecs, but adds a rendering stage and still needs input delivery; secondary possibility only |
| OS virtual webcam | Can use native GPU capture paths, but violates the portability/deployment objective |
| JavaScript webcam compatibility shim | Possible for cooperating templates; not native device registration and not selected |
| Chrome extension native messaging | JSON messaging to a separate process, not GPU media injection |
| Historical Pepper/Native Client plugins | Obsolete baseline; not an available route to adopt |
| Fake camera/file command-line switches | Useful test sources, not a live external-frame injection contract; changing-file/named-pipe tricks not selected |

Native WebRTC can use application-produced frames, but CEF does not expose its own internal peer-connection factory.
See the [upstream native example](https://webrtc.googlesource.com/src/+/refs/heads/main/examples/peerconnection/client/conductor.cc).
Other references: [canvas capture](https://www.w3.org/TR/mediacapture-fromelement/),
[native messaging](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging),
[Native Client retirement](https://developer.chrome.com/docs/native-client), and
[fake capture switches](https://github.com/chromium/chromium/blob/152.0.7977.134/media/base/media_switches.cc#L229).

## Miximus integration implications

Use ordinary sampled-texture input interfaces on the CEF node and keep its ordinary texture output. Input names must
respect the existing prohibition on duplicate interface names; the page's numerical input index is an adapter over
those native ports, not a new graph data type. GPU conversion, export pools, stream subscription, browser generations
and native GPU bridge details belong inside the CEF module. App state wires subsystem ownership and lifecycle. New
resource-sharing helpers must remain contained. This exploration grants no permission to change graph traversal,
`tick_one_frame()` ordering, the render snapshot, existing submission behavior or other nodes; any deviation requires
explicit user approval.

The node would act both as a browser-output source and as a consumer feeding the page. A subscribed input may need
evaluation even when the browser output is disconnected. The existing `prepare()` execution-demand mechanism is a
candidate for this; submission and execution still follow normal graph traversal. Browser callbacks must not resolve
upstream nodes, mutate the render snapshot, or execute the graph. Preserve cycle rejection rather than introducing
implicit browser feedback edges.

### Correct frame handoff

The initially suggested description, “`complete()` pushes pixels into a buffer for CEF,” needs a distinction between
CPU lifecycle and GPU readiness. Current NDI/DeckLink outputs record conversion in `execute()`, register a deferred
output through `app->defer_output()`, and publish the readback target after successful native submission. Workers obtain
readable pixels only after readback completion and any required noncoherent-memory invalidation.

That is the existing CPU-transfer ownership pattern, not a proposal to add CPU browser delivery. `complete()` is a
CPU lifecycle hook, not a GPU-completion signal; it must not expose unfinished transfer memory or recycle resources still in use. GPU delivery
similarly needs a bounded export lease published after successful submission,
producer completion before Chromium reads, and consumer completion before slot reuse. Neither the GPU proposal nor
the historical CPU route calls for changing the existing graph/render lifecycle. See
[NDI output](../src/nodes/ndi/output.cpp), [DeckLink output](../src/nodes/decklink/output.cpp),
[bounded readback](../src/gpu/transfer/texture_readback.hpp), and [GPU/media ownership](gpu-and-media.md).

### Timing and stable stream identity

Evaluation N can enqueue input for later browser consumption while the CEF output uses an already available browser
frame. Do not promise a same-evaluation round trip or fixed one-frame delay. Input buffering depth, target latency,
repetition/drop policy and source alignment remain deferred, subject to bounded memory and no render-thread stalls.

Sending a program timestamp in a `VideoFrame` or completing a generator write does not establish which later CEF paint
contains that input. Track submitted, delivered and observed identities separately if exact correlation is investigated.
Resize and source changes need generations so old queued frames do not reappear after a new source becomes active;
the page's stream/track identity can remain unchanged while old native pools retire.

Scope access to this node's connected inputs and approved page contexts. Revoke subscriptions on navigation/removal.
Receiving application-supplied frames avoids OS camera permission prompts, but does not imply every loaded page or
iframe should gain access to every source. Browser audio is a separate scope; no audio architecture is selected here.

## Open questions and possible future experiments

No experiments below are scheduled by this document. Updating the exploration is not implementation approval.

1. Prove Linux GPU import and a completed GPU copy into Chromium-owned storage using a small bounded pool. Verify
   producer readiness and safe export-slot reuse without CPU pixel readback or encoding/decoding.
2. Connect that frame to a per-page track. Compare the smallest JavaScript `VideoFrame` adapter with direct native
   pushable-source delivery before choosing the maintained API boundary.
3. Exercise stream creation before connection, source replacement, resize, alpha/color, stopped or retaining consumers,
   navigation, teardown and GPU-process loss. Keep the stream identity stable where its context remains valid.
4. Measure HD/UHD and multiple inputs: payload/copy costs, frame age, graph deadline misses and retained memory. Then
   qualify Windows/macOS independently. No performance or portability guarantee follows from source inspection.
5. Review the actual patch footprint and upgrade burden after the prototype. Consider removing the extra GPU copy only
   if the measured benefit justifies coupling export-pool reuse to browser-consumer release synchronization.

The stock CPU route and binary-injection analysis remain historical alternatives, not prerequisites for this proposed
prototype. The revised direction is a persistent input-slot `MediaStream` fed through a contained, source-level GPU
bridge. It remains separate from the main CEF browser-source plan and does not authorize changes to Miximus's
established rendering structure.
