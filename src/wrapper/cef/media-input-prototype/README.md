# Native media-input prototype

The media-input patch is now part of the regular CEF source build. Normal application builds include it.
This directory retains isolated staging and focused test tools for diagnostics. They use the standard
`source-build.json` revision and patch digests, with no separate prototype manifest; see [the standard workflow](../README.md).
The CEF patch reuses Chromium's native push source, media tracks, SharedImage importer and GPU raster copies.
It adds one private versioned C ABI and a native-handle Mojo method; generated public CEF classes are unchanged.
The separate Chromium patch only registers a focused test target and updates a test mock for CEF's existing
navigation-throttle signature change. No production Chromium source is changed by this experiment.

```sh
python3 src/wrapper/cef/media-input-prototype/build.py prepare --work-dir build-cef-source
python3 src/wrapper/cef/media-input-prototype/build.py test --work-dir build-cef-source --jobs 6
python3 src/wrapper/cef/media-input-prototype/build.py build --work-dir build-cef-source --jobs 6
cmake --build build -j
python3 src/wrapper/cef/media-input-prototype/build.py stage --work-dir build-cef-source \
  --baseline-runtime build/cef --destination build-cef-media-input
VK_LAYER_PATH="$PWD/build/tools/vulkan-validation/1.4.357.0/x86_64/share/vulkan/explicit_layer.d" \
MIXIMUS_VULKAN_VALIDATION=1 LD_LIBRARY_PATH="$PWD/build-cef-media-input/link" \
  build/src/nodes/cef/cef_media_input_probe "$PWD/build-cef-media-input/runtime" /tmp/miximus-media-input-profile
```

Use a fresh profile for qualification. Build/test bound both Ninja and LLVM concurrency using CPU affinity.
Use the standard source-build workflow to package the application SDK. Isolated staging below is optional.
The stage command removes baseline provenance and records the experimental patch/library digests instead.

Current scope: Linux single-plane RGBA8 sRGB with encoded-premultiplied alpha, main document only, eight independent native track sources, three
Chromium destination slots per source by default. `MIXIMUS_CEF_MEDIA_INPUT_DEPTH=1..8` selects a diagnostic
capacity before helper startup; this is a native setting, not a page-controlled resource request.
`MIXIMUS_CEF_MEDIA_EXPORT_DEPTH=1..8` independently selects the diagnostic node export depth (default two).
Session byte budgets still apply. Increasing export depth from two to three did not improve the measured six-input HD case.
The probe takes an optional input count (1–8). Without another argument it serializes one Vulkan export per input.
An additional `ASYNC_EXPORT_DEPTH` argument (1–8) runs a 60 Hz producer and separate transfer worker, for example
`.../cef_media_input_probe RUNTIME FRESH_PROFILE 8 2`. Three Chromium destinations are the provisional asynchronous
default; the two-slot serialized result does not generalize to the asynchronous case. External Vulkan producer completion precedes send; a completed GPU query
precedes reuse acknowledgement. Chromium destination reuse waits for the media frame's release SyncToken.
IPC/context loss never grants external reuse. The browser node integrates this bridge when the private sender is present;
normal builds require the qualified source-built SDK.
The five native source tests and one/eight-input browser-copy/playback probes pass on the development host.
See the implementation plan for measured results and limits.
Stopped-track reacquisition with a live clone is covered by both native and browser checks.
The probe verifies distinct per-input midtones and alpha on a transparent page, including a fully transparent case.
The real-session probe `cef_media_input_session_probe RUNTIME FRESH_PROFILE` covers eight inputs, source replacement,
disconnect-to-transparent, independent resize, reload and shutdown. Add `--small-inputs` to verify
1×1 through 129×127 inputs, including odd and non-square sizes, GPU pixels and every video element's logical dimensions. The private send ABI includes a monotonic source generation
and a metadata-only invalidation packet. Superseded GPU copies still retire safely but cannot push into the native source.
Node/session integration includes bounded workers, shared admission reservations, quarantine, demand and status.
Navigation/crash stress, adapter checks, HD/UHD cadence measurements and platform ports remain qualification work.
