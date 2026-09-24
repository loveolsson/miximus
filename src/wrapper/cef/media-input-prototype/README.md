# Native media-input prototype

Opt-in experiment atop the pinned revision-2 CEF source build. The production SDK/provenance is unchanged.
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
Do not run the baseline source prepare/package workflow on the experimental tree or label its output as revision 2.
The stage command removes baseline provenance and records the experimental patch/library digests instead.

Current scope: Linux single-plane RGBA8 sRGB, main document only, eight independent native track sources, three
Chromium destination slots per source. External Vulkan producer completion precedes send; a completed GPU query
precedes reuse acknowledgement. Chromium destination reuse waits for the media frame's release SyncToken.
IPC/context loss never grants external reuse. This is a diagnostic bridge, not yet production node support.
The initial native source tests pass; actual browser-copy/playback qualification is pending.
Track reacquisition while a clone remains live also needs qualification before node integration.
Pool-depth tuning, adapter checks, source-generation invalidation, global memory budgets, reconnect/crash recovery,
alpha/color qualification, node demand/status and platform ports remain qualification/integration work.
