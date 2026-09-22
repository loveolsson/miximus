# CEF dependency wrapper

Application builds never download or compile Chromium implicitly. The existing `sdk.json` identifies the stock
152.0.8 SDK used for baseline qualification; it is **not** a patched artifact. Linux/NVIDIA accelerated capture
failed with that artifact. Do not claim accelerated support from its successful initialization test.

## Approved source build

`source-build.json` pins CEF, Chromium, depot_tools, the bootstrap script digest, build arguments and both patch
digests. The user approved maintaining this patched Linux version on 2026-09-22. The source build remains based on
the selected stable CEF 152.0.8 / Chromium 152.0.7977.134 release.

The two changes are:

- `cef-linux-native-handle.patch`: upstream CEF commit
  [cafbf7f24971aa2526381e8e1978e11d1838ae64](https://github.com/chromiumembedded/cef/commit/cafbf7f24971aa2526381e8e1978e11d1838ae64),
  proposed in PR 4238. Select native-handle shared images on Linux; other platforms keep their allocation preference.
- `chromium-native-handle-capture.patch`: merged Chromium commit
  [9522ea8ad0bed3c3fd1b4f570ed9792dec8176d5](https://github.com/chromium/chromium/commit/9522ea8ad0bed3c3fd1b4f570ed9792dec8176d5).
  Permit native-handle GPU capture on Linux, correctly set the blit request's mappability flag, and accept the native
  result. Includes upstream regression tests and author attribution. Only diff path prefixes were changed, to match
  CEF's `git apply -p0` patch manager; code changes are unchanged.

Both patches apply to the pinned source. That is not yet evidence of a successful build or working GPU capture.
Their source licenses and upstream attribution are retained. No ABI, renderer scheduling, sandbox policy or Miximus
render path change is part of these backports. Remove the backports only after a stock stable SDK contains both
changes and passes the same hardware tests.

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

Stages stop on failure. `sync` uses CEF's pinned automation and shallow Chromium history; do not resync a prepared
tree because upstream sync can revert Chromium modifications. `prepare` registers the Chromium patch with CEF's
own patch manager and generates release projects. `build` is resumable. `test` builds and runs Chromium's frame-sink
capture tests, including the backported native-handle case. These tests complement, rather than replace, real CEF
GPU capture qualification.

The package stage emits a standard-layout release SDK, a `miximus-source-build.json` provenance file with the
`libcef.so` digest, and an archive digest. Builds use Chromium's pinned sysroot/toolchain and Ninja, omit debug symbols,
and retain official-build ThinLTO, PGO, control-flow integrity and sandbox support. The sync stage restores the
DEPS-pinned siso revision that upstream automation otherwise overrides with `latest`, and fetches the PGO profile.
Pinned inputs support reproducibility; byte-for-byte
reproducibility has not been established. The source build does not automatically replace the application's SDK.

Before promoting the artifact, verify the provenance and package digest, configure the application against that SDK,
and run the fresh-profile runtime and accelerated probes under Vulkan validation. Require actual accelerated delivery
and completed GPU copies; no software paint, CPU pixel fallback or sandbox disabling is acceptable. Adapter identity,
producer-fence publication, color conversion and lifecycle qualification remain separate requirements.
