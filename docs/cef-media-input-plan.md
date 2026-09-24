# CEF media input implementation plan

Started 2026-09-24 on `feat/cef-media-inputs`. Implements the GPU-only direction in
[the exploration](cef-media-input-exploration.md). Target: eight independent texture inputs per browser node;
first qualification: one 640×360 RGBA input. No CPU pixel fallback, codec, virtual webcam, graph lifecycle change,
or replacement of the qualified browser-output runtime.

## Architecture and provisional decisions

- Stable native ports `input_0` through `input_7`; page indices 0–7. The existing output remains `tex`.
- One main-document stream/track per slot, acquired asynchronously through
  `window.miximus.getInputMediaStream({inputIndex})`. Repeated calls share that slot's stream while its track is live.
  A stopped track can be reacquired. Source disconnect/resize does not stop the track. Navigation revokes access.
  Initial access is restricted to the browser node's main document, never arbitrary subframes or another node.
- Separate document, source/pool generation and per-submission identities. An old release must never free a new use
  of the same physical slot. Revocation prevents delivery but does not manufacture GPU completion.
- Render-thread execution records conversion into a bounded export lease. Successful submission publishes it;
  a worker establishes producer completion. CEF UI/renderer callbacks never evaluate graph inputs.
- First Linux transport uses native DMA-BUF handles and explicit layout metadata, transferred through Mojo.
  Never send descriptor numbers through JSON as if they were portable handles.
- Prefer a Chromium-owned GPU copy before media delivery. Export reuse waits for that copy's GPU completion;
  Chromium destination reuse also waits for media consumers and their GPU release dependencies. Both pools are bounded.
- Start with three slots per active input as an experiment, permit depths 1–8, and measure 1/2/3/4/8. These are capacity
  bounds, not a preroll requirement or a promise of N-frame latency. No free slot means a counted drop.
- Disconnected slots should eventually receive GPU-generated opaque black at a small initial size; choose exact cadence
  during playback qualification. Do not make acquisition wait for connected input or allocate eight full-size pools eagerly.
- Deliver native `media::VideoFrame`s through Chromium's existing `PushableMediaStreamVideoSource::Broker` and
  `MediaStreamVideoTrack`. Bind the track to JavaScript once. No per-frame JS `VideoFrame` wrapper, writable stream,
  or replacement media pipeline is needed. Keep page policy in Miximus. Do not patch device enumeration.

## Milestones and exit criteria

1. **Ownership foundation and feasibility.** Implement deterministic bounded admission for eight inputs, exact tokens,
   independent producer/consumer retirement and generation changes. Exercise exhaustion, cancellation, resize,
   navigation, delayed/duplicate release and one-slot operation without hardware. Verify local Vulkan DMA-BUF support.
2. **Export boundary.** Move the existing test-only export allocation into a reusable implementation helper, qualify
   renderable RGBA resources, and test repeated GPU-only export/import copies and foreign ownership handoff. Keep
   native handles private. Record adapter, format/modifier, dimensions and validation results.
3. **Small runtime patch.** Add native-handle transport and one narrow Blink adapter against the pinned CEF runtime.
   Keep the bridge opt-in/versioned and preserve stock builds. Compile changed targets before packaging. Prove one
   moving input with actual GPU-copy completion and a real page track. No claim of support from API presence alone.
4. **Node/session integration.** Add all eight native/web ports together; demand and resolve only subscribed inputs.
   Integrate frame-boundary publication, source replacement, independent resize and bounded asynchronous retirement.
   Unsupported builds must report media-input unavailability rather than silently accept unusable streams.
5. **Qualification and tuning.** Measure 1/2/4/8 active inputs, 640×360/HD/UHD, shallow pools, retaining consumers,
   paused video, rapid navigation, track stop/reacquire, browser/GPU failure, resize and shutdown. Separate input-copy,
   delivery and observed-paint identities. Report p50/p95/p99 occupancy/hold times, drops, GPU memory and end-to-end delay.
   Hardware results determine defaults; no theoretical claim that one or two slots suffice.

Each milestone is committed independently after relevant builds/tests. Hardware-dependent gates can remain explicitly
unqualified while deterministic lifecycle and patch/build work continues. A new machine is needed only when a concrete
allocation/import/synchronization failure prevents the next experiment.

## Upstream research, 2026-09-24

CEF master resolves to [`12add0082fa068571fd2167e83ed6b11dcc03f98`](https://github.com/chromiumembedded/cef/commit/12add0082fa068571fd2167e83ed6b11dcc03f98)
(commit dated September 22). Inspected current `cef_v8.h` and `cef_frame.h`; no external GPU media-frame constructor or
native-handle media transport was found there. GitHub issue searches for `MediaStream`, `external texture`, and custom
capture did not locate a matching media-ingress proposal. This is a limited search, not proof that none exists.
[Issue 1006](https://github.com/chromiumembedded/cef/issues/1006) and
[issue 2534](https://github.com/chromiumembedded/cef/issues/2534) concern browser rendering/output, not this input API.

Current Chromium main still contains `SharedImageInterface`, `MediaStreamVideoSource`, the native `VideoFrame` wrapper
and `MediaStreamTrackGenerator`. Their continued presence is not an internal ABI stability promise. Preserve a small
source patch, keep V8/Blink lifetime details in one adapter, and rebuild/qualify against each pinned upgrade. Public
Blink source/track helpers reduce dependency spread but do not themselves expose an arbitrary native frame as a JS object.
Sources: [shared images](https://github.com/chromium/chromium/blob/main/gpu/command_buffer/client/shared_image_interface.h),
[embedder source](https://github.com/chromium/chromium/blob/main/third_party/blink/public/web/modules/mediastream/media_stream_video_source.h),
[frame wrapper](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/modules/webcodecs/video_frame.h),
[generator](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/modules/breakout_box/media_stream_track_generator.cc).
The production pin remains CEF `1ce985cb23056548b9cc51483bbef4faf68b1cd3` / Chromium `152.0.7977.134`.

## Validation record

- Existing `gpu_dma_buf_vulkan_test`: all five tests passed locally with Vulkan synchronization validation on September 24.
  This includes exportable single-plane RGBA/BGRA and completed GPU copies across logical devices. The machine therefore
  passes this initial prerequisite; Chromium ingestion and eight-input throughput remain unproven.
- Ownership foundation: `cef_media_input_test` passes eight deterministic tests, covering all depths 1–8, eight inputs,
  generation invalidation, in-flight cancellation, stale/duplicate completion and concurrent navigation.
- Export boundary: `gpu::detail::dma_buf_export_s` allocates single-plane renderable RGBA DMA-BUFs and records GPU
  conversion plus FOREIGN/GENERAL handoff. The caller's bounded lease must still establish actual producer and consumer
  completion. Abandoned recordings never commit foreign ownership.
- `cef_media_input_vulkan_test` passes at 640×360: eight independent inputs at depths 1/2/3, four complete reuse rounds
  each (192 exported frames), changing GPU-generated colors, GPU-only pixel verification, and abandoned recordings
  before both initial use and reacquisition. No pixel readback occurs; only aggregate comparison counters reach the CPU.
  This serialized ownership test does **not** establish live throughput, latency or Chromium compatibility.
- Full native build, 30 GPU tests and 14 transfer GPU tests pass with Vulkan validation after the export changes.
- Opt-in prototype: all four `ExternalVideoSourceTest` tests pass in the pinned Chromium checkout: eight independent
  live tracks, unchanged GPU frame delivery to a normal track sink, native stop/clone/destruction behavior, and missing
  context rejection. The native application and browser probe build successfully. This does not yet qualify the GPU
  importer/copy/query or page playback on its own; those require the end-to-end probe.
- Reproducible CEF and test-only Chromium patches, exact revisions/digests, isolated staging, and qualification commands
  live in [the prototype workflow](../src/wrapper/cef/media-input-prototype/README.md). Its ABI remains experimental.

## Reuse decision after capture-path comparison

The Windows Media Foundation capture path already imports/copies GPU textures and uses the normal native media
pipeline. Its `DeliverTextureToClient()` and renderer `BindVideoFrameOnMediaTaskRunner()` currently require NV12;
they are not a drop-in RGBA/DMA-BUF entry point. The cross-platform `TextureVirtualDevice` is a separate Chromium
service that accepts SharedImages and registers an internal camera. Its buffer access notifications are useful but
its renderer branch still constructs frames with an empty release-mailbox callback in the pinned source.

The selected direct native source reuses `PushableMediaStreamVideoSource::Broker`, `MediaStreamVideoTrack`,
`MediaStreamSource`, `MediaStreamComponentImpl`, and `MediaStreamTrackImpl`. Frame delivery, sink fan-out, track
cloning/stopping and IO-thread dispatch therefore stay in Chromium. The only Blink-specific glue creates the track
and retains its existing broker. This can live in CEF's **existing** `blink_glue.cc`, which its standard patches
already compile into Blink's controller target; no additional production Blink public API is necessary.

Also inspected `WebGraphicsContext3DVideoFramePool` and `RenderableMappableSharedImageVideoFramePool`. They contain
useful copy/release-token machinery, but the former always converts to NV12 and permits a shared-memory GMB copy;
the latter is not an admission bound. Any reuse must explicitly reject CPU-backed ingress and cap total outstanding
allocations, including old sizes. The existing GPU-only copy and SharedImage primitives remain the intended boundary.

Source references (pinned Chromium):
[Windows GPU capture](https://github.com/chromium/chromium/blob/152.0.7977.134/media/capture/video/win/video_capture_device_mf_win.cc),
[renderer capture](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/platform/video_capture/video_capture_impl.cc),
[virtual-device contract](https://github.com/chromium/chromium/blob/152.0.7977.134/services/video_capture/public/mojom/virtual_device.mojom),
[native push source](https://github.com/chromium/chromium/blob/152.0.7977.134/third_party/blink/renderer/modules/breakout_box/pushable_media_stream_video_source.h).

## Initial browser qualification, 2026-09-24

The isolated prototype runtime compiled and the first real browser probe passed on this machine with Vulkan
validation enabled. At 640×360, one Vulkan export allocation was reused for 120 frames, all admitted by the native
push source. GPU comparisons verified both full-frame red and green after the normal `<video>` path and accelerated
browser output. No CPU pixel ingress/readback was used. Chromium had three bounded copy destinations.

Send-to-safe-reuse wall time was p50 3,457 µs, p95 3,788 µs, maximum 77,732 µs. These are **serialized diagnostic**
measurements including UI/IPC scheduling and contention with blocking output verification, not GPU-copy timings or
proof of sustained 60 Hz performance. They demonstrate that the basic path works here and justify continuing
multi-input and shallow-destination-pool experiments before integrating the render loop.

With eight inputs at the same size and three destinations, the initial uniform-color probe admitted 960/960 frames
and verified the complete eight-video output (p50 4,069 µs, p95 8,408 µs, maximum 44,548 µs). The probe was then
strengthened to assign a unique RGB pattern to each input and require two ordered complete output patterns. GPU-only
comparisons use each panel's exact horizontal region, so missing, duplicated or misrouted inputs cannot pass merely
because another video is painting.

Destination-depth experiments (one export allocation per input):

| Chromium slots/input | Active inputs | Result |
| --- | --- | --- |
| 1 | 1 | Only 1/120 admitted; the video retains its current frame and prevents reuse. Safe drops, no overwrite. |
| 2 | 1 | 120/120 admitted; both phases displayed. p50 3,409 µs, p95 3,710 µs. |
| 2 | 8 | 960/960 admitted; both **distinct per-input** patterns verified. p50 3,333 µs, p95 8,257 µs. |

Two destinations are a measured lower working bound for this serialized playback case, not yet the production
default or a guarantee for retaining consumers, asynchronous 60 Hz operation, HD/UHD, or another driver.
The comparator change passes all 30 GPU, 14 transfer, five DMA-BUF and one eight-input export/reuse tests with validation.
The native source suite now passes five tests, including stopped-track reacquisition while a clone remains live.

The browser probe now also encodes linear premultiplied graph colors into sRGB premultiplied RGBA8 before export,
then decodes accelerated browser output back to linear for comparison. Eight-input midtone, varying-alpha, and
fully transparent patterns pass at destination depth two (960/960 admitted). A separate GPU test checks the sRGB
linear branch, midtone reference, premultiplication, and zero-alpha behavior independently of the decoder.

Final runtime check: all eight page streams were acquired twice (same stream identity), their original tracks were
stopped with live clones, replacement tracks remained live, and both per-input midtone/alpha patterns then passed.
960/960 frames were admitted at depth two; send-to-reuse p50 3,280 µs, p95 7,028 µs, maximum 73,901 µs. The final
native build is clean, and 31 GPU + 14 transfer + five DMA-BUF + one export/reuse + eight ownership tests pass with
validation. This remains a probe: browser graph ports and asynchronous frame-boundary submission are the next milestone.

## Asynchronous export ownership and first cadence experiment

`media_input_exports_s` now owns fixed export slots for eight inputs, with separate native-submission and graph-frame
commit decisions. The intended node call records through `app->commands()` and commits through `app->defer_output()`;
`complete()` is not a GPU fence. A transfer worker polls actual producer completion outside metadata locks. Per-input
serial order is preserved, and polling rotates between inputs. Configuration allocates off the render thread, waits
for old leases and retained allocations, and never revives an input revoked during allocation. Export storage has a
per-queue byte budget (128 MiB provisional); the largest single attempted allocation can temporarily exceed the
remaining budget before its actual dedicated-allocation size is known. The total configured allocation stays within it.
Session-level admission still needs to account for Chromium destination memory and quarantined generations.

Unproven consumer retirement disables the queue and retains its allocations in a runtime-owned quarantine. That owner
must outlive Chromium shutdown. A cancelled frame whose native work was already submitted still drains its producer;
an unsubmitted recording can release its reservation without claiming GPU completion. Eight GPU tests pass, including
aborted evaluation, held consumer/resize, stale allocation retention, unknown completion, eight-input bounds, and a
newer frame whose commit must not overtake an older undecided frame.

The probe's optional `ASYNC_EXPORT_DEPTH` argument runs a 60 Hz producer with a separate transfer worker. All cases
below used eight 640×360 inputs, 120 scheduled ticks, validation, distinct per-input midtone/alpha patterns, and the
normal accelerated browser output. These short runs include cold startup and expensive GPU output verification.

| Vulkan exports/input | Chromium destinations/input | Producer admitted / 960 | Native source admitted | Producer capacity drops |
| --- | --- | --- | --- | --- |
| 1 | 2 | 920 | 558 | 40 |
| 1 | 3 | 919 | 918 | 41 |
| 2 | 3 | 928 | 926 | 32 |
| 2 | 3, final rerun | 936 | 931 | 24 |

Every run passed the pixel checks. Three Chromium destinations are therefore the provisional asynchronous default;
two only proved sufficient for serialized delivery. Two Vulkan exports give some scheduling slack, with no preroll.
The final rerun reported HTML `presentedFrames` of 114–117 per input; rVFC callback counts are reported separately and
can be lower. Send-to-retirement observation was p50 4,277 µs, p95 10,794 µs, max 70,457 µs, including worker polling.
No sustained HD/UHD or clean steady-state benchmark is claimed. The default application still uses the original
qualified runtime; graph input ports and session integration remain the next step.
