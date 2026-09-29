# Miximus

Experimental C++20 node-based video mixer/compositor with Vulkan rendering and a Vue/Baklava web editor.

## Getting started

- **Windows clean clone and full-feature port:** [Windows development handoff](docs/windows-development.md).
  Includes dependencies, the custom CEF source build, CUDA transfers, SDK setup, known implementation gaps and
  hardware acceptance tests. Windows qualification is still in progress.
- **Build and development:** [Development guide](docs/development.md).
- **Runtime and graph design:** [Architecture](docs/architecture.md).
- **GPU/media ownership:** [GPU and media](docs/gpu-and-media.md).

Clone with `--recurse-submodules`, or run `git submodule update --init --recursive` after cloning. Vendor SDKs and
local build artifacts are not included; follow the platform setup guide rather than copying another machine's build.
