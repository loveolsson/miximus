# CEF dependency wrapper

Application builds never download or compile Chromium implicitly. The existing `sdk.json` identifies the stock
152.0.8 SDK used for baseline qualification; it is **not** a patched artifact. Linux/NVIDIA accelerated capture
failed with that artifact. Do not claim accelerated support from its successful initialization test.

## Approved source build

`source-build.json` pins CEF, Chromium, depot_tools, the bootstrap script digest, build arguments and both patch
digests. The user approved maintaining this patched Linux version on 2026-09-22. The source build remains based on
the selected stable CEF 152.0.8 / Chromium 152.0.7977.134 release.

The two upstream allocation/capture changes are:

- `cef-linux-native-handle.patch`: upstream CEF commit
  [cafbf7f24971aa2526381e8e1978e11d1838ae64](https://github.com/chromiumembedded/cef/commit/cafbf7f24971aa2526381e8e1978e11d1838ae64),
  proposed in PR 4238. Select native-handle shared images on Linux; other platforms keep their allocation preference.
- `chromium-native-handle-capture.patch`: merged Chromium commit
  [9522ea8ad0bed3c3fd1b4f570ed9792dec8176d5](https://github.com/chromium/chromium/commit/9522ea8ad0bed3c3fd1b4f570ed9792dec8176d5).
  Permit native-handle GPU capture on Linux, correctly set the blit request's mappability flag, and accept the native
  result. Includes upstream regression tests and author attribution. Only diff path prefixes were changed, to match
  CEF's `git apply -p0` patch manager; code changes are unchanged.

Revision 1 built successfully and passed all 168 selected frame-sink capture tests. Hardware probes delivered and
GPU-copied 120 frames at 640×360, HD and UHD, but exposed an empty producer reservation-fence snapshot. Revision 1
is therefore **not synchronization-qualified**.

Revision 2 adds the local `chromium-native-handle-completion.patch`. The native-handle upstream change disables the
completion wait associated with CPU-mappable capture. CEF supplies no acquire fence to the Linux callback, and the
tested NVIDIA path returned the kernel's boot-time stub fence when its DMA-BUF write fences were exported. The local
patch retains Skia's existing asynchronous GPU-finished callback before delivering Linux RGBA blit results, independent
of CPU mappability. It changes no allocation flags, public CEF ABI or Miximus rendering code, and performs no CPU pixel
access. It also waits for other Linux RGBA blit requests in this custom CEF build; non-blit results, NV12 and other
platforms retain their existing behavior. Revision 2 builds, passes all 168 capture regression tests, and completes
120 accelerated GPU copies at both HD and UHD on the local NVIDIA P2000 with Vulkan validation. Those checks do not
yet qualify other drivers, pixel color accuracy, browser lifecycle stress or the eventual node integration.

The separately listed `test_patches` entry updates Chromium's `MockDisplayClient` to match the cross-platform
`CreateLayeredWindowUpdater` declaration introduced by CEF's existing `viz_osr_2575` patch. This local compatibility
patch is applied only by the test stage and changes no production code.
Their source licenses and upstream attribution are retained. No ABI, renderer scheduling, sandbox policy or Miximus
render path change is part of these patches. Replace the custom build only after a stock stable SDK contains the
allocation/capture fixes and provides a qualified producer-completion contract for the native-handle callback.

Use a disk-backed directory with sufficient space for Chromium, its toolchain, dependencies and build outputs.
Do not use `/tmp` when it is a small tmpfs. See the upstream
[Linux prerequisites](https://chromiumembedded.github.io/cef/master_build_quick_start.html#linux-setup).
System package installation is separate; this script does not invoke sudo or modify the host's package set.

```sh
python3 src/wrapper/cef/source_build.py sync --work-dir "$PWD/build-cef-source"
python3 src/wrapper/cef/source_build.py prepare --work-dir "$PWD/build-cef-source"
python3 src/wrapper/cef/source_build.py build --work-dir "$PWD/build-cef-source" --jobs 6
python3 src/wrapper/cef/source_build.py test --work-dir "$PWD/build-cef-source" --jobs 6
python3 src/wrapper/cef/source_build.py package --work-dir "$PWD/build-cef-source"
```

The build targets the library, resources and sandbox required by the SDK; it does not build the GTK sample application.
Stages stop on failure. `sync` explicitly bootstraps the pinned depot_tools Python and uses CEF's pinned automation
and shallow Chromium history; do not resync a prepared
tree because upstream sync can revert Chromium modifications. `prepare` registers the Chromium patch with CEF's
own patch manager and generates release projects. `build` is resumable. `test` builds and runs Chromium's frame-sink
capture tests, including the backported native-handle case. These tests complement, rather than replace, real CEF
GPU capture qualification.

The package stage emits a standard-layout release SDK, a `miximus-source-build.json` provenance file with the
`libcef.so` digest, and an archive digest. Builds use Chromium's pinned sysroot/toolchain and Ninja, omit debug symbols,
and retain official-build ThinLTO, PGO, control-flow integrity and sandbox support. The sync stage restores the
DEPS-pinned siso revision that upstream automation otherwise overrides with `latest`, and fetches the PGO profile.
The build and test stages also limit their own CPU affinity to at most `--jobs` available CPUs. Ninja's job count
alone does not constrain LLVM's internal ThinLTO workers; LLVM respects this Linux affinity limit when Chromium
requests all available threads. This bounds worker concurrency, not total memory consumption, and does not disable
any optimization or change other processes' affinity. Choose the job count with memory headroom for the final link.
Pinned inputs support reproducibility; byte-for-byte
reproducibility has not been established. The source build does not automatically replace the application's SDK.

Packaging also emits `miximus_cef_linux64_native_handle_r2.json`, an acquisition manifest containing the actual
archive SHA-256, archive root and patch identities. It has no download URL until an artifact is deliberately published.
Use the local archive and its generated manifest to extract a verified SDK:

```sh
cmake \
    -DCEF_MANIFEST="$PWD/build-cef-source/distribution/miximus_cef_linux64_native_handle_r2.json" \
    -DCEF_ARCHIVE="$PWD/build-cef-source/distribution/miximus_cef_linux64_native_handle_r2.tar.bz2" \
    -DCEF_DESTINATION="$PWD/build-cef-sdk" \
    -P src/wrapper/cef/acquire.cmake
```

The destination must be new. The stock `sdk.json` remains the default acquisition manifest for baseline comparisons;
it does not silently acquire the custom build. Preserve the generated manifest and source provenance with each artifact.

Before promoting the artifact, verify the provenance and package digest, configure the application against that SDK,
and run the fresh-profile runtime and accelerated probes under Vulkan validation. Require actual accelerated delivery
and completed GPU copies; no software paint, CPU pixel fallback or sandbox disabling is acceptable. Adapter identity,
producer-fence publication, color conversion and lifecycle qualification remain separate requirements.

The private browser session is enabled only when the SDK's recorded source revision, patches and build arguments
match `source-build.json`, and its `libcef.so` matches the provenance digest. The independent diagnostic probes can
still run against a stock SDK. Library and provenance changes trigger CMake reconfiguration and verification.
