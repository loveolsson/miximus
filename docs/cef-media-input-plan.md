# CEF media input implementation plan

**Current build:** media inputs are included in the regular revision-4 CEF SDK and normal `./build/miximus` launch.
No `LD_LIBRARY_PATH` override is required. Earlier isolated-runtime commands below are historical qualification records.
See [the standard SDK workflow](../src/wrapper/cef/README.md) for packaging/configuration.

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

## Node/session integration and qualification

The browser node now exposes `input_0`…`input_7` in both native and web definitions, including CEF-disabled builds.
Only page-subscribed inputs demand graph execution. The node records through the existing frame recording and
publishes via `defer_output`, after successful submission. Native/web status reports availability, subscriptions,
committed frames, deliveries, drops, held export slots, actual export allocation bytes, reservation estimates and failures. A stock runtime reports the
input feature unavailable; browser output remains usable.

The session owns a transfer worker, two Vulkan exports per active input, and a runtime-wide admission reservation.
Allocation and producer-fence polling stay off the render thread. The session export limit is now 256 MiB (the
initial 128 MiB rejected some of eight HD inputs after driver allocation padding). It is supplemented by a
separate 2 GiB shared input reservation, conservatively allowing eight Chromium destinations per input at padded
high-water dimensions. This is an admission estimate, not measured driver memory. Actual export allocation bytes
remain bounded separately. Reservations survive unknown-retirement quarantine until CEF shutdown. Chromium slot
reuse still waits for release tokens, including destinations retained across resize.

Source replacement, disconnect and resize advance a per-input generation. Private send ABI **v2** transports this
identity and supports texture-free generation invalidation. Old copies can finish and acknowledge safe external
reuse, but cannot enter the native media source once its generation advances. This does not retract frames already
accepted by Chromium's media pipeline. Document identity separately rejects navigation-stale work. A disconnected
input receives opaque GPU-generated black at the last known dimensions (256×256 before a source is connected; see the disconnected-input correction below).
Subscriptions currently last until document revocation; stopping all page tracks does not yet remove graph demand.

`cef_media_input_session_probe` exercises the real subsystem/session and accelerated output, with GPU-only pattern
checks across eight 640×360 inputs and three 120-tick stages. The first validation run delivered 2,855 of 2,858 committed
frames with three transport drops. It verified source replacement, disconnect-to-black, independent resize to
320×180, page reload, and drained shutdown without Vulkan validation errors. Reservations stayed at 94,371,840 bytes.
The test checks native video dimensions as well as unique per-input midtone/alpha patterns. This is short functional
qualification, not an HD/UHD performance claim or exhaustive crash testing.

For the isolated artifact currently staged on the development host:

```sh
LD_LIBRARY_PATH="$PWD/build-cef-media-input-r5/link" build/miximus
# Standalone integration qualification; use a fresh profile directory:
VK_LAYER_PATH="$PWD/build/tools/vulkan-validation/1.4.357.0/x86_64/share/vulkan/explicit_layer.d" \
MIXIMUS_VULKAN_VALIDATION=1 LD_LIBRARY_PATH="$PWD/build-cef-media-input-r5/link" \
  build/src/nodes/cef/cef_media_input_session_probe \
  "$PWD/build-cef-media-input-r5/runtime" /tmp/miximus-media-session-new-profile
```

The default application runtime remains unchanged. The experimental artifact must contain the matching helper,
CEF library and generated resources; do not mix a new library with old `.pak`/snapshot files. Reproduction uses the
prototype build script and patches, not this machine-specific directory name. Remaining qualification includes
rapid navigation/renderer failure with work in flight, retaining consumers, adapter mismatch, longer cadence runs,
and HD/UHD memory/throughput tuning. Windows/native platform transports remain outside this Linux implementation.

### Application graph and HD follow-up

The new `scripts/test_cef_inputs.py` uses private application settings and a local page that reports native video
size and `requestVideoFrameCallback().presentedFrames`. All eight source nodes are demanded with **no browser-output
consumer**. The campaign applies live resize/disconnect/reconnect, six closely spaced reloads, disable/enable, and
normal shutdown. Pixel correctness remains covered by the separate GPU comparison probes; this test observes graph
execution and media presentation metadata. Its test-pattern sources upload their generated pattern once; the browser
input transport itself still uses GPU images only.

The first eight-input 1920×1080 run revealed two actionable limits:

- Actual exports occupy **157,286,400 bytes (150 MiB)** on this driver, so the provisional 128 MiB cap rejected inputs.
  Session admission now allows 256 MiB of actual export allocations. A separate status counter reports those bytes;
  it is distinct from the padded input/destination reservation estimate (713,031,680 bytes for this case).
- Navigation with work in flight can lose a Mojo completion reply. The queue correctly quarantines its buffers, but
  previously left inputs permanently failed in a healthy browser session. The node now treats a poisoned input queue
  as a session failure and uses its existing bounded restart/backoff policy. The old queue and reservation remain
  quarantined until CEF shutdown. Repeated failures can exhaust the shared admission budget; restart never bypasses it.

After that fix, the complete HD campaign passed with one automatic restart during reload stress and no Vulkan
validation errors. A 15-second steady observation on the Quadro P2000 measured **34.1–34.4 presented frames/s per input**
for eight 1080p sources, two Vulkan exports and three Chromium destinations. This is a functional/cadence result with
validation enabled and a 640×360 browser viewport, not a 1080p output benchmark or a promise of 60 Hz on this hardware.
The graph scheduled more work than the bounded transport admitted; drops are expected under this load.

```sh
VK_LAYER_PATH="$PWD/build/tools/vulkan-validation/1.4.357.0/x86_64/share/vulkan/explicit_layer.d" \
MIXIMUS_VULKAN_VALIDATION=1 LD_LIBRARY_PATH="$PWD/build-cef-media-input-r5/link" \
  python3 scripts/test_cef_inputs.py --width 1920 --height 1080 --steady-seconds 15
```

The serialized native transport probe additionally passes explicit v2 metadata invalidation: an older-generation
packet is safely rejected, then a current-generation packet is admitted. All 960 eight-input frames preceding that
check were delivered and the unique per-input GPU patterns matched.

Follow-up validation: all 114 core tests and eight export/ownership GPU tests pass; native and CEF-disabled builds
and the web build pass. The baseline runtime still passes browser-output replacement, disable/enable and shutdown,
and explicitly reports media input unavailable with zero input allocations. The next tuning target, selected by the
user, is **4–6 simultaneous 1080p60 inputs on the current GPU**; eight-port infrastructure remains required.

### Four-to-six-input tuning

The graph campaign now accepts `--inputs 2..8`, independent source/browser dimensions, and warmup/steady intervals.
`MIXIMUS_CEF_MEDIA_EXPORT_DEPTH=1..8` selects a **diagnostic** native export capacity before session creation; default
remains two. Pending storage stays bounded at eight slots per input and the reservation estimate includes the selected
depth. The existing Chromium destination override remains independent; default is three. No depth implies preroll.

Initial 15-second comparisons on the P2000, using 1080p inputs:

| Inputs | Browser viewport | Export / Chromium slots | Validation | Presented frames/s per input | Export allocation |
| --- | --- | --- | --- | --- | --- |
| 4 | 640×360 | 2 / 3 | on | 59.7–60.0 | 75 MiB |
| 6 | 640×360 | 2 / 3 | on | 41.8–42.4 | 112.5 MiB |
| 6 | 640×360 | 2 / 3 | off | 42.6–43.7 | 112.5 MiB |
| 6 | 640×360 | 3 / 3 | on | 41.6–42.1 | 168.75 MiB |
| 4 | 1920×1080 | 2 / 3 | on | 59.1–59.7 | 75 MiB |

Each also passed resize/disconnect/reconnect, reload recovery, disable/enable and shutdown. The third export adds
56.25 MiB for six inputs without a measured throughput improvement; it is therefore **not** the default. A GPU-utilization
sample during that experiment was 96%, consistent with GPU work limiting throughput rather than a lack of export slots.
The captured application scheduler reported no deadline misses or skipped frames during the third-slot and full-HD
four-input intervals. Input delivery still drops when the bounded transport cannot keep up. This does not establish
which individual GPU stage dominates; per-stage GPU timing remains a possible follow-up before changing the CEF patch.

A subsequent **30-second** interval after **five seconds of warmup**, with validation off, measured **59.998 frames/s
on all four 1080p inputs in a 1920×1080 browser viewport**. The complete lifecycle campaign passed afterward. This supports
four-input 1080p60 for this simple video/CSS workload on the current GPU; it does not extend the result to six inputs,
more expensive pages or consumers retaining frames. Defaults remain two exports and three Chromium destinations.

```sh
MIXIMUS_VULKAN_VALIDATION=0 LD_LIBRARY_PATH="$PWD/build-cef-media-input-r5/link" \
  python3 scripts/test_cef_inputs.py --inputs 4 --width 1920 --height 1080 \
  --browser-width 1920 --browser-height 1080 --warmup-seconds 5 --steady-seconds 30
```

## Promotion into the regular build

Source-build revision 3 now includes the media-input CEF patch in the standard production manifest. CEF-enabled
configuration rejects old/stock SDKs unless diagnostic-only admission is explicitly requested. The standard source
workflow builds and packages the library, resources and provenance; `package --no-archive` makes a local SDK without
unnecessary archive compression. The application build is configured against that SDK and stages everything into
its ordinary `build/cef` directory. No environment override is required.

Verified with `LD_LIBRARY_PATH` removed, isolated graph settings, and the exact manual URL
`http://127.0.0.1:7351/cef-inputs.html`: the process loaded `build/cef/libcef.so`, the page returned HTTP 200, all four
inputs subscribed and delivered 124 frames before the check, and shutdown completed without Vulkan validation errors.
The old revision-2 SDK was separately confirmed to fail normal configuration with explicit upgrade instructions.

## Disconnected-input correction from the user's real graph

The manual page requests four streams, while the user's graph initially connects only inputs 0 and 1. Running that
unchanged graph reproduced thousands of Chromium `eglCreateImage` / Skia-representation errors in 30 seconds, despite
normal browser input status and no Vulkan validation failures. An isolated size sweep reproduced EGL import failures
for 16×16, 32×32, 64×64 and 128×128 exports on this driver; 256×256 succeeded. Vulkan format/allocation acceptance alone
therefore does not establish Chromium import compatibility for small images.

Previously the disconnected initial extent was 16×16. It is now the qualified 256×256 size, retaining the last known
source dimensions for later disconnections. The tiny internal black source texture is not exported; the GPU scales it
into the export allocation. The unchanged current graph then ran for 30 seconds with **zero EGL import errors and zero
Vulkan validation errors**, four active streams, and normal shutdown. The user's saved settings were not modified.

The session probe now starts all eight inputs disconnected before connecting sources; the graph campaign supports
`--connected-inputs` and rejects EGL/Skia import failures in its log, not just Vulkan validation errors. Earlier
post-connection disconnect tests retained a previously qualified large extent, so they missed the tiny initial case.
Native source delivery counters are not proof of successful image import: Chromium's copy helper can clear a failed
source to black and set a GL error. General connected-small-texture compatibility and explicit import-error propagation
remain separate follow-up work; this correction addresses the reproduced disconnected-input failure.

## Full-graph NDI loopback investigation (2026-09-24)

The user's updated graph connects a static 1080p pattern and a 1080p NDI loopback receiver to the four-video page;
inputs 2 and 3 remain disconnected. The graph also runs another NDI receiver, NDI output, DeckLink input/output,
a multiviewer and screen output. All measurements use isolated copies of the saved settings and the normal packaged
CEF runtime. Performance runs disable validation, last 45 seconds and compare seconds 10–40. A visually unchanged
copy of the page reports `requestVideoFrameCallback` metadata; no pixel readback is used. GPU utilization is the
whole-device `nvidia-smi` counter, including the desktop (about 28% with Miximus stopped), not per-node GPU time.

Before correction, both NDI receivers and the NDI/DeckLink outputs sustained approximately 60 fps with no receiver,
upload or output drops. The application scheduler had no new missed deadlines or skipped frames. Nevertheless,
**all four browser videos presented about 30 fps**, including static and black inputs, while browser paints were
near 60 fps. Aggregate delivery/paint counters alone therefore obscure the visible problem.

Controlled variants isolated the load:

- Disabling the browser reduced median whole-device GPU utilization from 99% to 75%.
- Replacing the browser's NDI connection with the static pattern still used 99% and delivered only about 133 frames/s
  across four inputs, versus 122 frames/s originally. This was not an NDI-specific loss.
- Disabling browser and screen reduced utilization to about 70%.
- Disabling DeckLink output raised aggregate browser delivery to about 179 frames/s but still saturated the GPU.

The v210 output packer separately recomputed shared pixels across divergent per-word branches. It now converts
six pixels once per four-word group, retaining the same transfer function, chroma averaging, rounding, final-pixel
replication and row-padding behavior. Browser-disabled utilization measured 72% afterward, versus 75% before; this
small change alone did not solve browser cadence (about 31–32 fps with the full graph).

Temporary Vulkan timestamp instrumentation then identified unusually slow browser-export rendering: a median of
**1.34 ms per 1920×1080 export**, versus roughly 0.1–0.25 ms for individual measured media conversions. Allocation
inspection found the export selected memory type 1, flags 0, on the system-memory heap. The allocator used the first
compatible bit without considering memory locality. On this NVIDIA driver, compatible device-local VRAM is later
in the memory-type list (type 7). The export allocator now prefers compatible `DEVICE_LOCAL` memory, retaining the
compatible fallback on devices without such a type. DMA-BUF layout, Chromium-owned destination copies, completion
requirements and bounded pool depths are unchanged; no CEF/Chromium patch is needed.

With VRAM allocation, the same instrumented export took **0.15 ms**, and the two connected browser videos reached
about 59–60 fps while whole-device utilization fell to about 83%. These timestamps include the scoped Vulkan work
and synchronization, not Chromium's separate GPU work; the temporary profiler was removed before final validation.

Validation: full native build; 32 Vulkan device tests, 14 transfer tests and eight media-export tests under validation;
the real eight-input CEF session probe, including initially disconnected inputs, source replacement, resize, reload
and shutdown. The v210 tests include independent color references and a new two-row, non-16-byte-aligned padding
regression. All passed. The user's saved graph remains unchanged.

Final **uninstrumented normal-build** measurement (`build/integration-tests/ndi-loopback-observe-20260924-171321`):
all four videos presented **60.0, 60.0, 60.0 and 59.97 fps**; median whole-device GPU utilization was **81.5%**
(maximum 85%). There was one browser transport drop during the approximately 30-second steady interval, versus
roughly 3,500 originally. There were no new graph deadline misses/skips, NDI receiver/upload drops, or DeckLink output
drops; screen output recorded one repeat and one skipped interval. No EGL import errors were logged. High remaining
whole-device utilization includes the rest of the graph and desktop and is not evidence that every GPU cost has
been optimized.

The earlier six-input result was also limited by system-memory exports. Repeating the isolated graph campaign with
**six 1920×1080 inputs and a 1920×1080 browser viewport**, five seconds of warmup and a 30-second steady interval,
now presented **59.998 fps on five inputs and 59.932 fps on the sixth**. Only two additional transport drops occurred
in the steady interval. Resize, live disconnect, six reloads, disable/enable and shutdown passed afterward
(`build/integration-tests/cef-inputs-20260924-171427`). This supersedes the earlier 42–43 fps six-input measurement;
it does not establish six-input 60 fps for every full hardware graph. Export/Chromium depths remain 2/3.

## Cleanup after performance qualification (2026-09-25)

Source-build revision 4 keeps the proven GPU-copy transport, export/destination capacities, generation checks and
quarantine lifetimes. It reduces the custom integration at the following boundaries:

- Blink track creation and stopped-track reacquisition use `MediaStreamVideoTrack::CreateVideoTrack`, so Chromium
  owns the platform-track/component construction. The existing pushable-source broker still delivers native frames.
- The renderer bridge no longer caches a redundant `CefV8Value`: the source retains the Blink track, and callers get
  a wrapper on acquisition. The unused copy-completion slot argument is removed.
- Packaging includes the authoritative private C ABI header. Miximus uses aliases and `decltype` from that header;
  a small initializer supplies packet size, invalidation FD and source-generation defaults. The v2 wire contract
  is unchanged. Headerless old SDKs no longer build these input integrations, even with diagnostic provenance override.
- The copy-completion callback checks ordinary raster errors as well as context loss. An import/copy error logs a
  diagnostic and stops this bridge delivering frames until navigation creates a new bridge. Raster errors belong to
  the shared context, so this intentionally fails the entire bridge closed rather than claiming per-frame attribution.
  GPU-completed work can still acknowledge safe export retirement without claiming successful delivery. Error queries
  occur after the asynchronous GPU completion callback, not in the frame submission path.

The small fixed destination array is retained. Chromium's `SharedImagePool` limits cached available images, not total
outstanding allocations, and would still require our admission bound across retained frames and sizes. Similarly,
the Miximus metadata pool and exported allocations encode different responsibilities rather than interchangeable
caches. Switching to `MediaStreamTrackGenerator` would add writable-stream policy and track-lifetime questions for
little further reduction; the narrower track factory is sufficient.

The standalone media probe now supports `--reject-invalid-import` in place of its optional asynchronous depth. It
first verifies valid pixels and source-generation behavior, then sends an unsupported modifier and checks that it
is not delivered. Drivers may reject this by losing the SharedImage channel rather than completing a failed copy:
missing completion is not declared safe, and the probe retains the exporter through runtime shutdown. The production
session already handles this case through its bounded timeout/quarantine/restart path. `--size=N` instead qualifies
square export sizes through the actual Chromium import and output-pixel path.

Revision-4 qualification passed: all 168 capture tests and five native source tests; full native build without
warnings; 32 Vulkan device tests, 14 transfer tests, eight media-export tests and eight ownership tests; the real
eight-input session lifecycle probe under Vulkan validation. The unsupported-modifier probe confirmed lost completion
without delivery, retaining the allocation through shutdown. A separate 128×128 test reproduced an ordinary EGL/raster
import error: all 120 submissions were rejected for delivery with safe retirement, and the new explicit GL-error
diagnostic appeared once. Thus both failure classes were exercised on real hardware. Artifacts are under
`build/integration-tests/cef-cleanup-20260925`.

Device-local size requalification passed pixel checks at 16×16, 32×32 and 64×64. The 128×128 failure above shows
that support is not monotonic in dimensions; this does not qualify arbitrary small connected sources. Initial
opaque-black exports now use the specifically qualified **16×16** extent instead of the former 256×256 workaround.
The eight-input session probe passed from eight initially disconnected streams through connection, source replacement,
disconnect, resize, reload and shutdown. Initial eight-input budget reservation fell from 20 MiB to 5 MiB. The graph
campaign with four requested streams and two connected sources also passed with Vulkan validation and no EGL errors.
Later disconnections still retain their source's last dimensions.

Final revision-4 performance check: six 1920×1080 inputs in a 1920×1080 browser viewport, five seconds of warmup and
30 seconds measured, presented **59.998 fps on all six inputs**, with **zero additional transport drops** in the
steady interval. Resize, disconnect, reload, disable/enable and shutdown also passed. This uses the normal staged
runtime, without `LD_LIBRARY_PATH`, with Vulkan validation off for performance
(`build/integration-tests/cef-inputs-20260925-082547`). The post-completion error check therefore preserved the
qualified six-input 1080p60 workload on this GPU.

## Demand-driven stream lifetime (implementation plan, 2026-09-25)

Only an input with live tracks created by `miximus.getInputMediaStream` should request graph work.
Connections alone do not activate it. A surviving clone keeps the source active; stopping the last track,
collecting an abandoned source, navigation, or context destruction revokes demand. DOM removal alone
is not a stop signal. Replace the strong JavaScript stream and Blink track caches with weak references.
Use Chromium's native source stop/destruction hooks, with activation epochs to reject delayed callbacks.

Implement in these steps:

1. Report native source activation/retirement to Miximus, scoped to the document token. Feed that state
   into the existing `prepare` demand mask and recursive `submit` dependency selection. Preserve other
   graph consumers and the established independently running hardware capture services.
2. Inactive inputs must revoke pending deliveries, drain actual GPU work, and release idle exports and
   Chromium destinations. Consumer-held frames retire using their real release tokens; no fabricated
   completion, forced reuse, or render-thread waits. Retained old frames remain inside the capacity bound.
3. Active inputs without a source display transparent content. Use a small Chromium-owned transparent
   frame and the existing refresh-frame mechanism, rather than allocating/exporting a dummy Vulkan
   texture on every render tick. Send only when entering the disconnected state or on refresh demand.
4. Validate explicit stop, surviving clones, garbage collection, reconnect/reacquisition, stale callbacks,
   transparent alpha, zero unused-input submissions, freed allocations, and source branch demand. Allow
   1–10 frames of normal reacquisition latency; prefer freeing unused buffers over keeping pools warm.
5. Rebuild and stage the patched runtime for the regular build, run native/source tests and real-GPU
   lifecycle checks, then repeat the six-input 1080p60 performance campaign. Commit coherent milestones.

The reservation remains conservative while consumers may retain old Chromium frames. An inactive input
releases its reservation only after Chromium reports all destinations released and the native exports
have drained. Unproven foreign GPU access retains both allocation and reservation through quarantine.

### Implementation

Revision 5 replaces one-way subscription with document-scoped native activity/retirement messages. A small
subclass of Chromium's existing pushable source reports stop/destruction and services refresh requests;
Chromium still owns track fanout, clones, and ordinary media delivery. Activation epochs ignore a delayed
old-source callback after immediate reacquisition. Both the JavaScript stream cache and Blink track/source
references are weak. The existing browser-node demand mask already gates upstream submission, so no new
scheduler mechanism is needed.

The private v3 sender distinguishes GPU frames, generation invalidation, and transparent-content control.
Disconnected live inputs use Chromium's stock 16×16 transparent I420A frame and refresh requests. This
synthetic neutral frame uses Chromium's normal media upload path; actual source pixels still stay on the
GPU. There is no Vulkan placeholder texture, startup GPU wait, disconnected export pool, or recurring
placeholder copy. Source changes and disconnects invalidate older in-flight deliveries.

Inactive export pools release allocations only after native completion and external leases drain. Idle
Chromium destinations are destroyed with their release sync tokens; consumer-held destinations remain
bounded and retire when released. An inactive input's reservation is returned after destination retirement
and export drainage. Context revocation remains conservative about reservations while old document frames
may still be held. A worker allocation that races a stop/source change is revoked before it can stay active.

The source qualification suite now includes last-clone retirement exactly once, abandoned-track collection,
and transparent refresh after the original frame was sent before a sink attached. All eight source tests
and the 168 existing capture tests passed before packaging the runtime. Real-GPU qualification follows below.

### Revision-5 qualification

The regular build now uses `miximus_cef_linux64_native_handle_r5`, staged into `build/cef`; these checks used
no `LD_LIBRARY_PATH` override. The full native build completed without warnings, and `git diff --check` passed.
With Vulkan synchronization validation enabled, 32 device tests, 14 transfer tests, nine export tests, and
eight ownership tests passed. The standalone eight-input native probe delivered all 960/960 frames, verified
both GPU pixel patterns, and rejected an invalidated source generation before accepting its replacement.

The eight-input session probe verified transparent output pixels for disconnected inputs, with **zero submitted
frames, zero export bytes, and zero reserved bytes** for eight initially disconnected live streams. Connection,
source replacement, disconnect, resize and reload passed. A surviving clone retained only its input's demand;
stopping it cleared demand, export bytes, occupied slots and reservations. The observed stop/release transitions
completed within 5 ms after the script reply. Reacquiring a disconnected stream allocated no native export buffers.

A four-stream graph with two connected sources also passed under validation. Its connected streams resumed in
about 52 ms and its disconnected streams in about 2 ms. The graph harness now stops all tracks, verifies that
input buffers/reservations are released and **demanding/submitted/executed node counts reach zero**, checks that
submission counters stay fixed, and reacquires the streams before testing resize, disconnect, reload and disable.

Performance checks used five seconds of warmup and 30 seconds measured, a 1920×1080 browser viewport and 1080p
sources, with Vulkan validation disabled. Four inputs presented **59.83–60.00 fps**, with seven additional transport
drops; reacquisition took **31–32 ms**. Six inputs on the repeat run presented **59.93–59.96 fps**, with 12 additional
transport drops; reacquisition took **49–50 ms**. Neither measured interval had graph deadline misses or skipped
frames. Capacities remain two exports and three Chromium destinations per input.

The first six-input run was slower: **56.86–59.46 fps**, with 405 additional transport drops, despite no graph
deadline misses. The unchanged-code repeat above was substantially better. This remains observed run-to-run
variation, not proof of unconditional six-input 60 fps; no larger buffers were introduced to conceal it. All runs
passed stop/free/reacquire, resize, disconnect, rapid reload, disable/enable, and shutdown.

Artifacts: `build/integration-tests/cef-lifetime-20260925`; mixed graph `cef-inputs-20260925-092543`;
six-input runs `cef-inputs-20260925-092636` and `cef-inputs-20260925-092843`;
four-input run `cef-inputs-20260925-093005` (all graph directories under `build/integration-tests`).


## Merge-review corrections (2026-09-25, revision 6)

The diagnostic staging tool now reads the standard `source-build.json` instead of maintaining a second revision and
patch-digest manifest. Its prepare check passes against the revision-6 source tree.

Renderer import/copy failures now send a document-token-scoped error to the native session. A failed session withdraws
input demand and enters the existing bounded browser restart policy. Buffer safety remains independent: completed
copies can retire safely even when delivery failed; missing completion still quarantines allocations. The node retains
the input error in status during failure. Before changing export layouts, the original two-input 128×128 graph verified
visible errors, zero active inputs on failure, exactly three retries, terminal failure and clean shutdown.

The small-image failure came from selecting NVIDIA's first advertised modifier, which uses a 256-row block even for
tiny images. Vulkan accepted those allocations, but EGL rejected their import. Export selection now skips NVIDIA block
heights larger than the logical image height (with one GOB as the minimum). Driver ordering for larger images and other
vendors is preserved. Pixel dimensions, GPU-only transport, capacities, completion and quarantine contracts are unchanged.
Linux builds use the system libdrm headers for modifier definitions, discovered in the Vulkan wrapper.

Revision 6 was built, packaged and selected as the regular SDK in `build/cef`. Validation on the Quadro P2000 passed:

- Full native build without warnings, 156 ordinary tests, C++ formatting and `git diff --check`.
- All 168 Chromium capture tests and eight native media-source tests.
- 32 Vulkan device tests, 14 transfer tests and nine media-export tests with synchronization validation.
- Normal and `--small-inputs` eight-input session probes, checking GPU pixels and both logical dimensions of every
  video element through connection, source replacement, disconnect, resize, reload, clone retirement and reacquisition.
  Small inputs include 1×1, 7×3, 8×8, 16×16, 32×32, 64×64, 128×128 and 129×127, plus a 63×7 resize.
- The original two-input 128×128 graph now passes stop/free/reacquire, independent resize, disconnect, rapid reload,
  disable/enable and shutdown with no import errors.
- The invalid-modifier probe verified valid pixels first, then rejected delivery and retained the export through CEF
  shutdown when the driver dropped completion. No unsafe retirement was introduced by failure reporting.

Logs are in `build/integration-tests/review-merge-fixes-r6`; the graph run is
`build/integration-tests/cef-inputs-20260925-134548`. The failure/retry check before the layout fix is preserved under
`build/integration-tests/review-input-failure-r6`. These checks do not constitute a new full DeckLink/NDI/display or
cross-vendor qualification campaign.

## Navigation retirement and mutable stream cache (revision 7)

Document revocation now retains the old document token for destination-retirement acknowledgements. These travel
over CEF's process-level browser-manager channel because navigation destroys the old frame's message pipe. The browser
checks the renderer process identity; the native session only removes known retirement accounting. Live activity
still uses the current main frame. The CEF frame
releases its bridge when the context ends; outstanding copies and VideoFrames retain it until they finish. Native
sessions track outstanding document tokens per input, accept retirement without reviving old demand, and wait for
previous documents before allocating replacement GPU inputs. A page that navigates away without acquiring new inputs
can therefore return its admission reservation after export and destination retirement.

The JavaScript cache also weakly remembers the native input track. Acquisition reconstructs the stream if page code
removes that track or substitutes/adds tracks, without keeping abandoned sources alive. The session probe covers track
removal, foreign-track substitution, and navigation from eight connected GPU inputs to `about:blank`, where demand,
held buffers, exports, and reservations must all return to zero.

Revision-7 validation passed the full native build, all 158 ordinary tests, nine GPU export tests, and the session
probe at normal and small input sizes with Vulkan synchronization validation. Both stream-mutation checks passed.
Navigation from eight 640×360 inputs returned the 90 MiB admission charge, exports, and demand to zero in 66 ms;
small-input navigation retired in 65 ms. The regular six-input 1920×1080 graph passed stop/reacquisition, independent
resize, disconnect, rapid reload, disable/re-enable, and shutdown. Each input presented 59.99 frames/s during the
five-second steady interval; reacquisition took 45–47 ms. No synchronization-validation errors were reported.

## Cross-origin retirement, admission recovery, and timestamps (revision 8)

A renderer process swap can shut down the old process before frame-release callbacks arrive. The patched browser
manager now tracks active media-input documents on the UI thread and observes their renderer process lifetime.
Explicit destination retirement and process termination both retire document accounting through the process channel.
Neither path acknowledges native DMA reads: unproven exports still fail closed and retain their quarantine lease.
A delayed retirement acknowledgement cannot erase an input that the current document has already reacquired.

Admission growth is now transactional. Failed or cancelled configuration rolls back its tentative increment while
preserving high-water charges for earlier successful generations. Export-capacity and shared-budget errors retry
with a 250 ms backoff; unsupported dimensions and other configuration errors remain failed until their source changes.

Transparent control messages carry the program timestamp. Chromium creates a fresh transparent frame for refresh
requests, advances its timestamp by elapsed time, and keeps the next connected frame monotonic on the same track.
The session probe checks this with a real `MediaStreamTrackProcessor` through disconnect.

Revision-8 checks passed the native build, all 158 ordinary tests, ten Vulkan export tests, and the normal session
probe under synchronization validation. The cross-origin test navigated from `127.0.0.1` to `localhost`, returned
all 90 MiB of reservations within 70 ms, and resumed all eight streams on history-back. The 4096×4096 budget probe
admitted two inputs, rejected the other six without retaining tentative charges, then recovered each waiting pair
as its predecessors stopped. All reservations returned to zero at the end.

Reproduce the additional regressions with the qualified SDK staged in `build/cef`:

```sh
python3 scripts/test_cef_media_input_navigation.py
build/src/nodes/cef/cef_media_input_session_probe build/cef /tmp/miximus-budget-profile --budget
```

Enable Vulkan synchronization validation as described in the GPU guide when qualifying hardware. These runs do not
extend the prior cross-vendor or DeckLink/NDI/display qualification scope.
