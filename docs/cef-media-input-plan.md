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
- For the first native adapter, compare a GPU-backed JS `VideoFrame` plus the existing track generator against a native
  push source. Keep page policy in Miximus; expose the smallest runtime operation needed. Do not patch device enumeration.

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
