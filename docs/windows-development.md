# Windows setup and full-feature port handoff

## Objective and current status

The Windows task is **full feature parity**, including the custom source-built CEF browser, GPU texture inputs to
Chromium, CUDA/Vulkan transfers, DeckLink, NDI, screen outputs, text/fonts, and the web editor. CEF and CUDA are
required work in this port even though their CMake switches are optional for other builds. A build with either
feature disabled is only an intermediate diagnostic checkpoint, not completion.

This guide was checked against the repository on 2026-09-29. The manifest dependencies install successfully on
Windows and pass an isolated MSVC C++20 compile/link/runtime check, including a local WebSocket echo exchange,
Boost.Fiber, and all eight requested FFmpeg libraries. All 93 installed WebSocket++ headers match the documented
Linux binary package byte-for-byte. Windows configuration now reads the NDI runtime DLL's version resource;
the full-feature port commands remain a target workflow. Existing Linux results do not qualify Windows. Some Windows implementations
already exist (DeckLink COM/MIDL, font discovery, monitor discovery, Unicode paths); CEF and CUDA still have explicit
Linux restrictions and require implementation, not just library installation.

Read [development](development.md), [architecture](architecture.md), [GPU/media](gpu-and-media.md),
[frame timing](frame-timing-and-synchronization.md), and the nearest `AGENTS.md` before modifying their subsystems.
Preserve Linux support while adding Windows paths. Put SDK discovery/linkage in `src/wrapper/`.
The Windows preset opts into vcpkg; ordinary Linux CMake configuration continues to use its existing dependencies.
Windows validation does not substitute for a Linux build when shared source or build logic changes.

The Windows Release checkpoint now configures and builds successfully, including the bundled web UI and validated
SPIR-V shaders. All 199 ordinary CTest tests pass, and `miximus.exe --help` launches with the staged DLLs.
NDI SDK 6.3.2.0 was verified through both its DLL version resource and `NDIlib_version()`.
Linux-style FFmpeg discovery was checked with clean and stale-cache fixtures, and the existing NDI version-file
parser was checked with supported, too-old and malformed versions. These are discovery checks, not a Linux build.
CUDA Toolkit 13.4 is detected; its Windows transfer implementation and the custom Windows CEF port remain pending.
With the installed SDK 1.4.357.0 validation layer, all 32 device tests and 14 Vulkan staging-transfer tests pass
using binaries/shaders rebuilt with glslang 16.4.0 from that SDK. All five shaders compiled and passed `spirv-val`,
and all 199 ordinary tests passed again. These runs isolated third-party implicit layers via
`VK_IMPLICIT_LAYER_PATH` pointing to an empty directory: stale TikTok LIVE Studio registry entries otherwise
reported two missing layer manifests per device. Khronos synchronization validation remained enabled via
`VK_LAYER_PATH=C:\VulkanSDK\1.4.357.0\Bin` and `MIXIMUS_VULKAN_VALIDATION=1`; no registry entries were changed.
This does not qualify CUDA, CEF, physical DeckLink/NDI I/O, or screen presentation.
The wrapper now requires glslang >=16.2.0, preserving support for the existing Linux compiler. Windows was tested
with 16.2.0 previously and 16.4.0 after this change; Linux was not rebuilt here. Cached SDK paths were refreshed
to 1.4.357.0. Logs are in `build-win/sdk357-{configure,build,ctest,device,transfer}.log`.

## Machine and toolchains

Use a native Windows x64 checkout and an interactive desktop with a Vulkan-capable GPU. Full CUDA qualification
requires an NVIDIA GPU and driver supporting the selected CUDA toolkit and Vulkan external memory/semaphore sharing.
DeckLink acceptance requires suitable hardware and the Desktop Video driver; NDI acceptance requires a sender/receiver.
WSL builds do not validate Windows DLL loading, COM, D3D sharing, presentation, or CUDA Win32 interoperability.

Install the following before starting the port:

| Dependency | Installation and repository requirement |
| --- | --- |
| Git for Windows | [Installer](https://git-scm.com/downloads/win); initialize every submodule at its recorded commit. |
| Visual Studio C++ tools | Install Desktop development with C++, x64/x86 MSVC tools, CMake tools/Ninja, and MFC/ATL for Chromium. MSVC 2022 v143 is a starting point for Miximus and the vcpkg baseline below. Chromium has its own toolchain requirements; install side by side as needed. |
| Windows SDK and debugging tools | The locally inspected pinned Chromium source calls for Windows 11 SDK **10.0.26100.7705** and Debugging Tools **10.0.26100.3323 or newer**. Its packaged toolchain is VS 2026 18; the toolchain script also recognizes VS 2022. Use the pinned source's requirements, not a moving Chromium-main guide. MIDL must be available for DeckLink. |
| CMake and Ninja | CMake **3.28+** and Ninja on `PATH`. Use Ninja initially to keep executable paths independent of build configuration subdirectories. |
| Node.js/npm | Install a current Node 22 release **at least 22.12**, or a compatible newer LTS. `package.json` says 22+, but the locked Vite dependency requires at least 22.12 on that line. Use `npm ci`. |
| Python | Python **3.11+** for repository scripts (`source_build.py` uses `hashlib.file_digest`). Chromium also supplies its own pinned Python through depot_tools. |
| Vulkan SDK | [LunarG Windows SDK](https://vulkan.lunarg.com/sdk/home): headers/loader >=1.3, `glslangValidator` **16.2.0 or newer**, `spirv-val`, Vulkan tools, and Khronos validation layer. Verify the installed compiler; configuration reports its version and path. |
| CUDA Toolkit | Install the [NVIDIA Windows CUDA Toolkit](https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/index.html), including runtime development headers/import libraries and tools. Select a toolkit whose compiler/driver support table matches the installed MSVC and GPU. Record the exact toolkit and driver versions. `CUDAToolkit_ROOT` can select a nondefault installation. |
| Blackmagic DeckLink SDK | Download **Desktop Video SDK 16.0** from [Blackmagic support](https://www.blackmagicdesign.com/support/family/capture-and-playback); this is the repository's current SDK baseline. Install the separate Desktop Video driver for runtime testing. The old README's 12.1 baseline is obsolete. |
| NDI SDK | Obtain the Windows **NDI SDK 6.2 or newer** from [NDI developers](https://ndi.video/for-developers/ndi-sdk/). Miximus needs headers, x64 import library, and the SDK runtime DLL with its version resource; NDI Tools alone is insufficient. |
| NVIDIA Video Codec SDK | Obtain the [Video Codec SDK](https://developer.nvidia.com/nvidia-video-codec-sdk/download). The local SDK inventory is **13.1.15**. This wrapper is separate from CUDA transfers; include it in full dependency discovery, but its successful discovery does not establish an implemented encoder/decoder feature. |
| Custom CEF/Chromium | Build the pinned, patched Windows SDK as described below. A stock downloaded CEF SDK is not a replacement for the custom media-input API. |
| Open-source native libraries | Install through vcpkg as below. Submodules already supply stb, fiberpool, sanitizers-cmake, magic_enum, GoogleTest, spdlog, Volk, and VMA; do not replace these with unrelated installed versions. |

For Chromium, use a short ASCII path on a local NTFS SSD, for example `C:\cef`. Reserve substantial additional
space beyond Miximus: upstream CEF's starting guidance is 150 GB for a debug tree and 32 GB+ RAM recommended.
Allow extra space for release/test outputs and SDK archives; cap parallelism for available memory.
See [CEF Windows setup](https://chromiumembedded.github.io/cef/master_build_quick_start.html#windows-setup).
The pinned Chromium files `docs/windows_build_instructions.md` and `build/vs_toolchain.py` are authoritative after
sync; SDK servicing version 10.0.26100.7705 still uses the `10.0.26100.0` include/lib directory name.

## Clone and install native packages

Run application commands in **Developer PowerShell configured for x64 MSVC**, from the repository root unless noted.
Use native Windows CMake/Ninja, not MinGW/MSYS compiler binaries. Stop at every nonzero native-command exit code;
PowerShell does not automatically stop for these errors.

```powershell
git -c core.autocrlf=false clone --recurse-submodules https://github.com/loveolsson/miximus.git C:\src\miximus
Set-Location C:\src\miximus
git config core.autocrlf false
git config core.longpaths true
# If the Windows work is on another branch, switch to it before updating submodules.
git submodule update --init --recursive
git submodule status --recursive
git rev-parse HEAD
```

Clone the branch/commit containing this handoff. Preserve LF bytes in CEF patch files: their SHA-256 values are
verified by the source builder. A clean clone does not contain local `3rd-party` SDKs, `build/tools`, CEF build
artifacts, `node_modules`, or the developer's settings. Obtain them explicitly; do not copy a Linux build cache.

The checked-in `vcpkg.json` lists packages and `vcpkg-configuration.json` pins the default registry to commit
`ef7dbf94b9198bc58f45951adcf1f041fcbc5ea0` (tag `2025.06.13`), including GLFW 3.4 and FFmpeg 7.1.1
with `postproc`. The registry reference also selects that tag, so version history does not depend on a moving HEAD.
The manifest installs the directly used Boost packages, FFmpeg components, FreeType, GLFW,
GLM, nlohmann-json, websocketpp and zlib. Submodule dependencies and vendor SDKs remain separate.

Boost packages use a separate registry pin at `66c0373dc7fca549e5803087b9487edfe3aca0a1`
(tag `2026.01.16`), selecting Boost **1.90.0** consistently across all Boost modules. The other libraries
retain the default registry pin, including FFmpeg 7.1.1 with `postproc`.

Use the vcpkg bundled with Visual Studio 2026, or an existing standalone vcpkg installation. There is no need
to clone another copy when Visual Studio already provides it. Set `VCPKG_ROOT` for the current shell:

```powershell
# Run from the Miximus checkout in an x64 Developer PowerShell.
$vsInstall = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$env:VCPKG_ROOT = Join-Path $vsInstall 'VC\vcpkg'
# For standalone vcpkg, set VCPKG_ROOT to that checkout instead.
& "$env:VCPKG_ROOT\vcpkg.exe" install --triplet x64-windows --host-triplet x64-windows
if ($LASTEXITCODE -ne 0) { throw 'vcpkg installation failed' }
& "$env:VCPKG_ROOT\vcpkg.exe" list

cmake --preset windows-release
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
cmake --build --preset windows-release --parallel
```

`CMakePresets.json` uses Ninja, MSVC, Release, `build-win/` and the vcpkg toolchain. Both the explicit install and the preset
share the ignored `vcpkg_installed/` directory in the checkout; downloaded sources and compiled packages are
not committed. vcpkg builds missing binaries from source for both Release and Debug, then reuses its binary
cache on subsequent matching installs. The FFmpeg manifest features select development libraries without the
command-line programs. CMake also installs missing manifest packages automatically. The preset sets the CMake 4
compatibility floor needed by older submodules before their first `cmake_minimum_required()` call.
Machine-specific overrides belong in the ignored `CMakeUserPresets.json`.

**Boost/WebSocket++ compatibility:** Upstream main documents the working Linux installation as Boost
**1.90.0** (Ubuntu package `1.90.0-6ubuntu1`) and patched WebSocket++ package
`0.8.2+git20250909-2` (headers report **0.8.3-dev**). Its distribution changelog identifies upstream
PR **1190** as providing the modern Boost.Asio fixes.

The overlay under `src/wrapper/vcpkg-ports/websocketpp/` downloads that exact distribution source archive
and packaging archive, verifies their SHA-512 hashes, and applies the complete, unchanged `1190.patch`
from the package's patch series. It contains no locally rebased Asio or C++20 patches. See its README
for provenance and checksums. The Windows Boost release matches Linux, but the vcpkg recipes are not
the same as Ubuntu's packaging patches or ABI. Stock WebSocket++ 0.8.2 is not an equivalent replacement.

This is the ordinary dependency/bootstrap configuration, not full Windows feature acceptance: CEF retains
its default OFF, and CUDA retains its existing Linux-only implementation gate. The full-feature configuration
below still requires the custom CEF SDK and platform port. Installation success does not establish application
or hardware correctness.

Sources: [vcpkg manifest/CMake integration](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration),
[pinned FFmpeg port](https://github.com/microsoft/vcpkg/blob/2025.06.13/ports/ffmpeg/vcpkg.json).

Use `x64-windows` consistently (dynamic CRT/libraries); avoid mixing x86, static-CRT, MinGW, Debug and Release
artifacts. FFmpeg's `postproc` feature selects GPL code in this baseline. The current wrapper requests avcodec,
avformat, avdevice, avutil, avfilter, swscale, postproc and swresample, including development headers/import libraries;
an `ffmpeg.exe` download alone is insufficient. Fontconfig, X11/Xrandr and libdrm are Linux/non-Windows dependencies,
not required Windows packages.

## SDK layout and environment

Extract the complete DeckLink SDK so this file exists:

```text
C:\src\miximus\3rd-party\decklink-sdk\Win\include\DeckLinkAPI.idl
```

A real directory is fine; a symlink is not required. Alternatively, create a directory junction to an extracted
SDK root (adjust the target, and do not replace an existing populated directory):

```powershell
New-Item -ItemType Directory -Force .\3rd-party
New-Item -ItemType Junction -Path .\3rd-party\decklink-sdk -Target 'C:\SDKs\Blackmagic DeckLink SDK 16.0'
```

The SDK wrapper generates `DeckLinkAPI.h` and `DeckLinkAPI_i.c` with `midl /env amd64`, and links Ole32/OleAut32.
Do not download unrelated generated headers. Place/junction the Video Codec SDK at `3rd-party\video-sdk` with
`Interface\nvcuvid.h`, `Lib\x64\nvcuvid.lib`, and `Lib\x64\nvencodeapi.lib`.

Set NDI to the actual installed SDK root:

```powershell
$env:NDI_ROOT = 'C:\Program Files\NDI\NDI 6 SDK'
Test-Path "$env:NDI_ROOT\Include\Processing.NDI.Lib.h"
Test-Path "$env:NDI_ROOT\Lib\x64\Processing.NDI.Lib.x64.lib"
([System.Diagnostics.FileVersionInfo]::GetVersionInfo("$env:NDI_ROOT\Bin\x64\Processing.NDI.Lib.x64.dll")).ProductVersion
Get-ChildItem $env:NDI_ROOT -Recurse -Filter Processing.NDI.Lib.x64.dll
```

Windows SDKs need not contain `Version.txt`. The wrapper reads the numeric product version from the matching
SDK DLL under `Bin/x64` (or `Bin/x86`) using PowerShell and
[FileVersionInfo](https://learn.microsoft.com/en-us/dotnet/api/system.diagnostics.fileversioninfo).
It does not load a DLL from `PATH` or execute target code. Linux/macOS retain the existing `Version.txt` check
(first line containing a version such as `v6.2.0`); all platforms require at least 6.2.
This checks the installed SDK runtime, not which DLL an eventual process loads. Record the actual runtime DLL
directory and prepend it to `PATH` for launching the app and tests.

After installing Vulkan/CUDA, open a new developer shell and check:

```powershell
Get-Command cl.exe, midl.exe, cmake.exe, ninja.exe, node.exe, npm.cmd, python.exe
$env:VULKAN_SDK
$env:CUDA_PATH
& "$env:VULKAN_SDK\Bin\glslangValidator.exe" --version
& "$env:VULKAN_SDK\Bin\spirv-val.exe" --version
& "$env:VULKAN_SDK\Bin\vulkaninfo.exe" --summary
& "$env:CUDA_PATH\bin\nvcc.exe" --version
nvidia-smi
```

Ensure Vulkan `Bin` is on `PATH`. The wrapper accepts glslang 16.2.0 or newer; `MIXIMUS_GLSLANG_VERSION` is the
minimum version, not an exact pin. `Vulkan_GLSLANG_VALIDATOR_EXECUTABLE` can select a specific compiler.
After replacing an SDK, clear cached discovery paths with
`cmake --preset windows-release -U 'Vulkan_*' -U MIXIMUS_SPIRV_VAL`, rebuild all shaders and rerun GPU validation.
Newer compilers can change SPIR-V output, so record the compiler used for each qualification. The shader compiler and validation layer can come
from separate SDK versions; the Linux validation notes identify problems with the older 1.4.341 layer. Qualify the
installed Windows layer independently and record its version.

## Required custom CEF build and Windows implementation

Start with [the wrapper README](../src/wrapper/cef/README.md), [browser design](cef-browser-sources.md),
[media-input plan](cef-media-input-plan.md), and [implementation progress](cef-implementation-progress.md).
The checked-in source inputs are:

- `src/wrapper/cef/source-build.json`: revision **9**, exact CEF/Chromium/depot_tools revisions, automation digest,
  GN arguments, production and test patch digests.
- `src/wrapper/cef/sdk.json`: CEF **152.0.8+g1ce985c+chromium-152.0.7977.134**, API **15200**. Its URL, checksum and
  platform describe a stock **Linux** SDK; they must not be reused for a Windows custom artifact.
- `src/wrapper/cef/source_build.py`: resumable `sync`, `prepare`, `build`, `test`, `package` stages. It currently
  rejects Windows. Port the stages and manifest selection, preserving reproducibility and Linux behavior.
- `patches/cef-linux-native-handle.patch`, `chromium-native-handle-capture.patch`,
  `chromium-native-handle-completion.patch`, and `cef-media-input.patch` under the wrapper, plus the manifest's
  `test_patches`. Inspect every patch for platform scope rather than assuming a successful application implements
  its behavior on Windows.

The required result is a **Windows source-built SDK with the custom browser media-input functionality**, including
the authoritative `include/internal/cef_miximus_media_input.h`, its exported private functions, and matching binary
provenance. Preserve the semantic fixes for allocation, producer completion, native track demand, generation-aware
copy completion, transparent frames, and destination retirement. Linux-only capture patches may remain guarded;
Windows needs equivalent qualified behavior. Do not remove the custom input API or substitute CPU image transport.

Use this sequence for the source-build port:

1. Install the pinned Chromium toolchain prerequisites listed above. Bootstrap Windows depot_tools in **cmd.exe**,
   per [upstream instructions](https://chromiumembedded.github.io/cef/master_build_quick_start.html#windows-setup),
   with its directory first on `PATH` and `DEPOT_TOOLS_WIN_TOOLCHAIN=0`. Use its Windows Python/batch entry points;
   do not share this depot_tools tree with WSL. Pin depot_tools to the manifest revision and prevent auto-updates.
2. Download the automation script from the manifest's exact CEF commit, verify `automate_sha256`, and sync using the
   existing builder's `--branch`, `--checkout`, `--x64-build`, `--no-build`, `--no-distrib`, and pinned dependency
   options. Verify both source HEADs. Preserve DEPS-pinned tools/PGO profiles. Read the checked-out Chromium Windows
   requirements before building; do not use an unpinned master checkout just because the quick-start example does.
3. Add a platform-specific source-build manifest (or a platform selection in the manifest schema). Retain explicit
   Windows GN arguments, toolchain identity, patch digests and API version. Remove Linux `use_sysroot` assumptions;
   preserve the intended release/sandbox configuration. Adapt `ensure_bootstrap`, `python-bin/python3`, shell
   commands, Linux affinity calls and executable suffixes. Bound build/link concurrency for memory on Windows.
4. Apply and verify production patches in the correct CEF/Chromium roots. Preserve the prepared-tree guard against
   destructive resync. Port the custom media-input patch's native GPU handle representation, cross-process
   transport, Windows exports and import/completion logic. The current ABI has an `fd` field; changing its meaning
   silently or stuffing a Win32 `HANDLE` into it is not a compatible Windows implementation. Version the private
   ABI and package the matching header when its layout changes.
5. Generate CEF's x64 release projects and build Windows `libcef`, resource and sandbox targets using the pinned
   toolchain. The existing `chrome_sandbox` target is Linux-specific: select Windows targets from the pinned GN
   files and include `cef_sandbox.lib` as required by the CEF Windows integration. Build/run the capture and native
   media-source regression tests from the manifest, porting their platform assumptions as necessary.
6. Package a standard Windows SDK using CEF's distribution tooling, including headers, `libcef.dll` and its import
   library, wrapper sources, required sandbox library, resources/locales, ICU/snapshot data and runtime DLLs.
   Copy the authoritative custom media-input header. Produce Windows artifact metadata, exact patch/build identity,
   the **Windows DLL's** SHA-256, and archive checksum; retain licenses. Do not relabel `libcef.so` provenance.
7. Port `src/wrapper/cef/CMakeLists.txt` and `acquire.cmake` as needed to verify/select the Windows artifact, build
   the helper, and stage a complete usable runtime. Replace ELF `RUNPATH`, `dladdr`, `libcef.so` and symlink logic
   with deliberate Windows loading/staging. Resolve private API exports on Windows and preserve sandbox startup
   for browser/subprocesses. Keep Chromium's bundled Vulkan/ANGLE loader files from accidentally replacing the
   application's Vulkan loader. Do not enable `MIXIMUS_CEF_ALLOW_UNQUALIFIED_SDK` for acceptance.
8. Port `src/nodes/cef/` runtime, renderer, capture and media-input export paths and their CMake platform guards.
   Browser-to-Miximus capture must import the actual D3D shared texture handle type, match DXGI/Vulkan adapters,
   establish producer completion and retain borrowed resources until the GPU copy is complete. Miximus-to-browser
   inputs need Windows GPU export/import and cross-process handle ownership with completion acknowledgements,
   bounded pools, stale-generation rejection and safe navigation/process retirement. A contained D3D11 GPU bridge
   is acceptable under the existing design; CPU readback/software paint is not.

NT shared handles, legacy KMT handles and opaque Vulkan handles are different contracts. Follow the
[Windows import qualification rules](cef-browser-sources.md#platform-import-qualification); do not assume a keyed
mutex protocol or treat D3D `Flush` as GPU completion. The current DMA-BUF helpers in `src/gpu/detail/` are Linux
implementations, not Windows interoperability support.

No ready-to-run Windows source-build command exists yet. The Windows agent should implement this workflow and
replace this paragraph with the verified commands, manifest, artifact name and test results once it works. The
normal application configure must not silently download/build Chromium.

## Required CUDA transfer port

Read [CUDA transfers](cuda-transfers.md) and [the DeckLink direct-memory contract](decklink-direct-memory.md).
Installing CUDA is necessary but not sufficient: `src/wrapper/cuda/CMakeLists.txt` only creates `cuda_dependencies`
on Linux today. The Windows task includes enabling discovery/linkage **and implementing Win32 interoperability**.

- Update the wrapper, GPU capability/allocator code in `src/gpu/device.cpp`, and
  `src/gpu/transfer/detail/cuda_transfer.cpp`. Existing memory/semaphore capability queries, export flags and calls
  use `OPAQUE_FD`, `vkGetMemoryFdKHR`, `vkGetSemaphoreFdKHR`, and POSIX close semantics.
- Implement matching Vulkan Win32 external-memory/semaphore queries, extensions, export calls and CUDA import
  descriptors. Define owned-handle RAII and failure cleanup according to the selected NT handle contracts; Linux
  FD ownership rules cannot simply be reused. Match the CUDA and Vulkan devices by identity.
- Preserve direct imports of the actual RGBA8 image (including mipmaps) or padded v210 word buffer, dedicated
  allocations where required, external queue ownership, per-slot imports, completion events, and host alignment/
  registration. No intermediate CUDA device frame or SDK-to-staging CPU copy.
- Preserve startup qualification of every required upload/readback representation. Staging remains the ordinary
  default and `--use-cuda` requests CUDA. Once CUDA is selected, per-stream fallback is forbidden. Windows CUDA
  acceptance must demonstrate actual CUDA uploads **and** readbacks; startup fallback on an incapable device is a
  separate behavior test, not proof of working CUDA support.
- Port the verification/benchmark runners' executable paths, process handling and shell assumptions. Video Codec
  SDK discovery does not enable these transfers, and NVIDIA DVP is not needed for this implementation.

## Configure, build, and load the runtime

Install web packages before native bundling:

```powershell
Push-Location web
npm.cmd ci
npm.cmd run build
npm.cmd test
Pop-Location
```

The following is the **full-feature configuration target**, to use after preparing the patched Windows SDK and
implementing the platform gates above. At the current commit it is expected to fail at the CEF Linux-only check;
CUDA can otherwise report unavailable even with `MIXIMUS_ENABLE_CUDA=ON`. Those are porting tasks, not missing flags.
Set `$cefSdk` to the actual packaged Windows SDK root; the example path is an intended location, not a supplied artifact.

```powershell
$cefSdk = 'C:\cef\distribution\miximus-cef-windows64'
cmake -S . -B build-win -G Ninja `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  "-DVCPKG_INSTALLED_DIR=$PWD/vcpkg_installed" `
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  "-DNDI_ROOT=$env:NDI_ROOT" `
  "-DCUDAToolkit_ROOT=$env:CUDA_PATH" `
  -DMIXIMUS_ENABLE_CUDA=ON `
  -DMIXIMUS_ENABLE_NVIDIA_VIDEO=ON `
  -DMIXIMUS_ENABLE_CEF=ON `
  "-DMIXIMUS_CEF_ROOT=$cefSdk" `
  -DMIXIMUS_CEF_ALLOW_UNQUALIFIED_SDK=OFF `
  -DMIXIMUS_TUNE_NATIVE=OFF `
  -DBUILD_TESTING=ON
cmake --build build-win --parallel
```

A separate `build-win-core` directory with CEF/CUDA/Video Codec discovery temporarily disabled can isolate compiler
and basic runtime problems while porting; keep that result labelled partial. DeckLink and NDI currently have no
corresponding disable switches. Do not finish the Windows task at this checkpoint.

Use a fresh build directory when changing compiler, architecture or toolchain. If using Visual Studio or Ninja
Multi-Config instead, pass `--config RelWithDebInfo` to builds and `-C RelWithDebInfo` to CTest, and account for the
configuration subdirectory in all executable paths.

For a single-config RelWithDebInfo developer run, prepend the vcpkg release runtime directory and the discovered
NDI/CUDA runtime directories to the same shell's `PATH`:

```powershell
$ndiRuntime = 'C:\Program Files\NDI\NDI 6 SDK\Bin\x64' # Verify against the installed SDK.
$env:PATH = "$PWD\vcpkg_installed\x64-windows\bin;$ndiRuntime;$env:CUDA_PATH\bin;$env:PATH"
.\build-win\miximus.exe --help
```

For Debug use the matching vcpkg `debug\bin`. Windows CMake targets stage their linked project/SDK DLLs, including
`static_files` and NDI, before linking so GoogleTest discovery can load them. vcpkg also stages transitive DLL imports
from its installed tree. Use `dumpbin /dependents` on executables/DLLs when diagnosing loading, including generated
build tools and test binaries. Newly added executables using project/SDK DLLs should call
`miximus_stage_runtime_dlls`. Follow the qualified CEF runtime layout from its Windows port, not a blanket addition
of CEF's directory to global `PATH`.

## Likely first build issues

| Symptom | Inspect/fix |
| --- | --- |
| FFmpeg discovery | Each component now uses independent `FFMPEG_<component>_*` cache variables on every platform; Windows additionally selects the matching Debug/Release import library. Old shared `INCLUDE_DIRS`/`LIBRARIES` entries are ignored. |
| Missing `postproc` | Confirm the chosen FFmpeg build supplies the requested development component. The proposed vcpkg baseline includes it; any removal from Miximus needs an explicit dependency cleanup, not a fake success. |
| Missing Boost headers | Check all directly used header packages as well as Fiber, Program_options and URL; the install list includes Asio, Container, Describe, Locale, Mp11 and UUID (used by the asset bundler). |
| MIDL not found / DeckLink header errors | Use the x64 developer environment, correct SDK root and full IDL set. Confirm generated files are dependencies of every consumer. |
| MSVC errors in Windows-only code | Check `font_registry_win.cpp`, monitor and COM code, const correctness, Win32 macro collisions, Unicode paths, and required Windows system libraries. Fix target/platform ownership rather than globally weakening diagnostics. |
| Native build succeeds but web UI is absent | Native bundling can report web failure only as a final warning. Inspect `build-win/static/web_build_failed.txt`, `web/dist`, and npm invocation (`npm.cmd` on Windows). |
| Shader tool mismatch | Inspect the selected glslang path/version and `MIXIMUS_SPIRV_VAL`; clear stale tool paths on SDK changes. |
| CEF initialization succeeds but no browser texture/inputs | Check the patched Windows DLL/header/provenance, private exports, helper loading, adapter identity and GPU completion. A stock SDK or software rendering is not acceptance. |
| CUDA is requested but staging runs | Check the wrapper's platform gate and Win32 resource qualification. Require completed `cuda-vulkan-direct` transfers, not just CUDA Toolkit discovery. |

## Validation and completion gates

First run deterministic tests, then explicit GPU tests with the installed Windows validation layer available:

```powershell
ctest --test-dir build-win --output-on-failure
$env:MIXIMUS_VULKAN_VALIDATION = '1'
.\build-win\src\gpu\gpu_vulkan_test.exe
.\build-win\src\gpu\gpu_transfer_vulkan_test.exe
.\build-win\src\gpu\gpu_transfer_vulkan_test.exe --use-cuda --log-debug --gtest_repeat=3
```

Record every exit code. The CUDA run must log completed `backend=cuda-vulkan-direct` uploads and readbacks and no
`backend=vulkan-staging`. Port the log assertions in `scripts/test_cuda_transfers.sh` as well as its launch command.
If a requested validation layer is missing, fix discovery; do not silently rerun without validation. Ordinary CTest
does not register GPU hardware tests by default (`MIXIMUS_TEST_VULKAN=OFF`). `gpu_window_test` is Linux-only today;
Windows presentation needs its own runtime coverage.

Use private settings, initially an empty graph, rather than `resources/settings.json`, which contains Linux paths,
monitor/device names and enabled hardware nodes. Do not overwrite an existing test graph on subsequent runs:

```powershell
if (-not (Test-Path .\build-win\windows-smoke.json)) {
  '{"schema_version":1,"nodes":[],"connections":[]}' | Set-Content -Encoding ascii .\build-win\windows-smoke.json
}
.\build-win\miximus.exe --settings .\build-win\windows-smoke.json --log-debug --stop-after 60
```

While it runs, open `http://127.0.0.1:7351/`. The API exposes `/api/v1/config` and `/api/v1/status`; capture these with
`Invoke-RestMethod` for diagnostics. Add a generated source and screen output, then save/restart using the same
settings. Test each feature and finally the combined graph:

1. **Core/editor:** graph edits synchronize between two clients, status updates work, settings survive restart,
   paths with spaces/non-ASCII characters work, text/fonts render, and timers/shutdown behave correctly.
2. **Screen:** windowed/fullscreen output, monitor changes, resize/reconnect, DPI scaling and sustained presentation.
3. **CUDA:** all channel layouts, padded pitches, v210, mipmaps, pinned alignment, bounded memory, exact upload IDs,
   retained/abandoned leases and teardown. Exercise both directions under validation; benchmark staging and CUDA
   separately with the ported `scripts/benchmark_cuda_transfers.py`.
4. **CEF:** custom SDK tests plus `cef_runtime_probe`, `cef_accelerated_probe`, `cef_session_probe`,
   `cef_subsystem_probe`, and media-input probes/tests. Port Linux-specific assumptions before using them as evidence.
   Require actual accelerated browser output and GPU-only browser texture inputs, pixel/alpha/color checks,
   multi-input load, stop/clone/reacquisition, transparent/disconnected streams, navigation beyond the process-ID
   regression boundary, browser crash/reload, bounded pools and orderly sandboxed subprocess shutdown.
   Port `scripts/test_cef_browser.py`, `test_cef_inputs.py`, `test_cef_load.py`, and
   `test_cef_media_input_navigation.py`; some assume Linux binary paths and signals. The manual input page is
   `http://127.0.0.1:7351/cef-inputs.html`.
5. **NDI and DeckLink:** discovery, input/output, format/mode changes, unplug/reconnect, alpha/keying where supported,
   frame-rate conversion, buffered cadence and sustained playback. Verify direct backend-owned DeckLink DMA leases
   with both staging and CUDA. Test NDI networking/firewall configuration on the intended local network.
6. **Combined graph:** CEF inputs/output, generated/text sources, NDI, DeckLink and screen presentation together,
   with staging and CUDA in separate runs. Record frame timing/drop counters, resource usage and shutdown behavior.
   Port the relevant `scripts/test_*` shell runners; do not claim Windows timing-soak coverage from Linux runs.

Format touched C++ with clang-format and web files with Prettier. Run the native build, CTest, `npm run build`,
relevant web tests and `git diff --check`. GPU/media/SDK work additionally needs the hardware evidence above.

Keep a Windows results record containing the Miximus/submodule/vcpkg commits, compiler/Windows SDK/Node versions,
all SDK versions, CEF source manifest/artifact hashes, GPU/driver/device identity, configure command, logs and exit
codes, private test graph, and observed failures/fixes. A feature without its hardware is **unverified**, not passed.
Full acceptance includes working custom CEF and CUDA on suitable hardware; documenting or disabling their current
platform gates does not complete the port.

## Suggested instruction for the Windows agent

> Read `AGENTS.md` and `docs/windows-development.md`. Set up this clean Windows x64 checkout and implement/test the
> full Windows port, including the pinned custom CEF source build and GPU browser inputs, CUDA/Vulkan Win32
> transfers, DeckLink, NDI, screen output and the web editor. Install all listed dependencies, port the current
> Linux-only build/runtime paths, preserve existing GPU ownership and direct-memory contracts, and continue beyond
> any core-only build checkpoint. Record reproducible Windows setup and hardware results in the docs. Do not treat
> disabled CEF/CUDA, a stock CEF SDK, CPU browser texture fallback, or CUDA fallback to staging as feature completion.
