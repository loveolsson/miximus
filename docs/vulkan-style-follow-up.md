# Vulkan style and containment follow-up

## Decision and scope

Documented against `vulkan-rewrite` at `2d9bf9c` following the style and containment review relative to the
handwritten OpenGL implementation on `main`. These are maintainability findings, not additional correctness findings
or evidence of measured performance regressions. None is a merge blocker on its own; the separate correctness and
hardware acceptance requirements still apply.

Address the structural work in a dedicated pass after both `vulkan-rewrite` and `cef-browser` have merged into
`main`. GPU ownership, transfer, drawing, and presentation code may be shared with CEF; changing these boundaries now
would create merge work and duplicate validation. Recheck all usage claims against the combined tree before removing
an API or implementation. Small mechanical cleanups could be done earlier, but this document changes no runtime code.

The goal is the project's existing simplicity: cohesive concrete classes, readable call sites, clear ownership,
and flexible definitions where they serve real consumers. Avoid replacing incidental complexity with a framework,
additional abstract interfaces, builders, or trivial forwarding layers. Preserve explicit recordings, completion
and lease ownership, plain typed parameter structs, and the separation of windows from presentation.

All eight items below remain open. The shader-layout item incorporates the subsequent performance discussion and
supersedes the original suggestion to move all packing to the recording boundary.

## 1. Contain GPU device internals

**Code:** [shared device state](../src/gpu/detail/device.hpp), [device setup](../src/gpu/device.cpp), and the
[GPU implementation directory](../src/gpu/detail/).

The shared internal header collects device discovery, queue/submission state, retirement, pipelines, resource
representations, and recording bookkeeping. Initialization, use, and destruction of those responsibilities span
multiple implementation files. File separation alone does not provide clear ownership boundaries.

Group cohesive responsibilities into small concrete internal components where that makes their lifecycle easier to
follow. Keep device state as the owner, with explicit dependencies between components. Do not merely replace shared
fields with getters or introduce a general backend abstraction.

Preserve queue serialization, independently owned recording contexts, submission ordering, deferred retirement,
startup pipeline preparation, and failure handling. Check CEF's resource import and completion dependencies before
moving state. Validate construction/unwinding, concurrent recording/submission, and shutdown as well as ordinary draws.

## 2. Reduce the presenter to the production contract

**Code:** [presenter API](../src/gpu/presenter.hpp), [implementation](../src/gpu/presenter.cpp), and
[screen adapter](../src/nodes/screen/detail/output_presenter.cpp).

The presenter supports both callback-driven frame selection and a latest-frame mailbox exposed through `publish()`.
At review time, screen output uses callbacks and mailbox publication is exercised by tests. Supporting both adds
state, branches, and metrics to the production implementation.

Check the merged CEF tree for mailbox consumers. If there is no production requirement, adapt tests to the callback
contract and remove the mailbox path and its unused metrics. If both contracts are required, document their distinct
consumers and contain their frame-supply behavior instead of interleaving it throughout presentation.

Preserve timestamp-based selection, pacing/feedback, lease retention, resize, GPU/WSI retirement, and latched screen
failure reporting. Do not add failure retries or change timing policy as part of this cleanup. Run display tests and
the screen-output failure integration check against the retained contract.

## 3. Keep probe-only GPU capabilities out of the application surface

**Code:** [recording API](../src/gpu/recording.hpp), [conversion implementation](../src/gpu/detail/conversion.cpp),
[pipeline creation](../src/gpu/detail/pipeline.cpp), and [GPU tests](../src/gpu/tests/).

The review found `pack_rgba`/`unpack_rgba` buffer conversion operations and `rgba16_float` support whose consumers
were probes/tests, while the production GPU API and pipeline setup still carry them. The application uses RGBA16
UNORM working textures; float test targets must not silently redefine that contract.

Recheck CEF and other production consumers, then remove unused capabilities or isolate genuinely useful probe
support. Prefer testing production paths where those tests can establish the same property. Keep specialized probes
when they provide distinct coverage, without making their formats or pipelines application startup requirements.

Preserve channel order, alpha/color precision, production conversion coverage, and startup preparation of pipelines
that live rendering actually uses. Update shader bundling and tests together; do not simply delete coverage to reduce
code size. Validate retained conversion results and capability rejection behavior.

## 4. Separate conversion resources from transfer-worker mechanics

**Code:** [shared transfer worker](../src/gpu/transfer/detail/transfer_worker.hpp) and
[transfer services](../src/gpu/transfer/).

The CRTP worker implements conversion-texture allocation through implicit requirements on stream fields such as
`config` and `conversion_texture`. This couples general task progression and memory accounting to a particular
UNORM16 rendering resource policy.

Move conversion-resource setup and release into a small explicit resource helper or the appropriate stream resource
owner. Keep the common worker focused on scheduling and bounded resource accounting. Avoid a new polymorphic resource
framework, and avoid duplicating allocation logic in each service.

Keep allocation and destruction on the resource worker, account for conversion memory, and preserve precision,
mipmap policy, exact upload IDs, external leases, and retirement order. Check CEF stream/resource needs before choosing
the helper boundary. Validate allocation failure/replacement, resizing, teardown, Vulkan/CUDA transfers, and the
[DeckLink direct-memory contract](decklink-direct-memory.md).

## 5. Make drawing options readable at call sites

**Code:** [drawing helpers](../src/gpu/drawing.hpp), [teleprompter](../src/nodes/teleprompter/teleprompter.cpp), and
[NDI nodes](../src/nodes/ndi/).

`draw_texture()` has nine positional arguments. Specifying a viewport can require spelling out unrelated default
opacity, conversion, compositing, and channel-order values. The call obscures which settings the node intends to change.

Use a small typed options struct with named/designated fields, or a small number of operation-specific calls. Keep
ordinary drawing, mixing, video encode/decode, and format packing understandable as operations. Alpha mode, channel
order, and clipping should remain settings where appropriate; do not create an API for every combination.

Preserve defaults, normalized geometry, clipping, alpha handling, and compositing. Migrate CEF and existing nodes
together. Validate representative draw/mix/conversion results and inspect call sites for accidental default changes.
Separate C++ entry points do not require additional shader variants or pipelines.

## 6. Preserve packed shader parameters while clarifying their boundary

**Code:** [parameter types](../src/gpu/recording.hpp), [construction helpers](../src/gpu/drawing.hpp), and
[private push-constant packing](../src/gpu/recording.cpp).

The original review flagged padded matrix arrays in `color_transform_s`, packing/transpose logic in the public
`color_parameters()` helper, and flat rectangle arrays alongside semantic geometry types. The concern is how much
layout knowledge a node needs, not whether data should match shader memory layout.

The old OpenGL setters performed named map lookups, runtime type checks, and individual uniform calls. The new typed
path avoids that work. This is a concrete implementation difference, not a measured claim about overall frame speed.
Keep typed, shader-ready data; do not reintroduce string-keyed uniforms or dynamic parameter maps for encapsulation.

The recorder already translates `draw_s` and `mix_s` into private push-constant structs with layout assertions.
`color_parameters()` already hides matrix padding and transpose conventions from normal callers. A prepacked
`color_transform_s`, constructed when configuration changes and reused each frame, is a reasonable API. Do not move
that work into every draw merely to hide its representation.

As part of item 5, clarify semantic construction and operation-specific settings. Keep raw offsets, padding rules,
and layout assertions in the GPU layer, and use existing geometry types where they make calls clearer without adding
per-frame work. Retain the packed representation wherever it already achieves this. Check CEF alpha/channel-order
requirements and preserve matrix orientation, color conventions, ABI checks, and configuration-time precomputation.
Validate color/alpha results; benchmark any proposed change that adds recurring packing or dispatch work.

## 7. Remove the redundant DeckLink renderer abstraction

**Code:** [output-path declarations](../src/nodes/decklink/detail/output_path.hpp) and
[implementation](../src/nodes/decklink/detail/output_path.cpp).

`output_frame_renderer_i` has one concrete implementation, and both output paths create that same renderer.
The additional interface and duplicate factories no longer express distinct rendering implementations.

After checking the merged tree, use a concrete renderer with the required format/configuration data and consolidate
identical construction. Keep the separate SDK output paths where they represent real pixel-format or frame-creation
requirements. Do not flatten the transfer backend or direct-memory extension boundary as collateral cleanup.

Preserve precomputed color parameters, stride/format handling, DMA addresses, SDK lease lifetime, and future DVP
extensibility. Check whether CEF introduces another renderer consumer; validate both ARGB and V210 output paths on
suitable hardware.

## 8. Remove mechanical migration leftovers

**Code:** [application header](../src/core/app_state.hpp), [node implementations](../src/nodes/), and
[test-pattern generator](../src/nodes/generators/test_pattern.cpp).

The review found unused `gpu/window.hpp` includes in non-screen nodes, the full device header pulled through
`app_state.hpp` where forward declarations may suffice, and an empty `if (rendered_frame_)` in the generator.

Remove unused includes and empty branches. Use forward declarations where the type's actual use permits them,
placing full includes in implementation files. Check CEF consumers for dependencies on transitive includes and give
those consumers explicit includes rather than retaining unnecessary coupling. Build all affected targets; do not
add tests that merely mirror these mechanical edits.

## Execution and acceptance

Start the post-merge pass by confirming each finding against the combined tree and recording any real consumers that
change its disposition. Land small, independently reviewable commits: mechanical cleanup and unused-path removal,
then drawing/resource boundaries, then larger internal ownership changes. Keep behavior changes separate. Mark items
resolved with the implementing commit and relevant validation, or explain why a retained abstraction is justified.

Follow [the development guide](development.md) and [GPU/media ownership rules](gpu-and-media.md). Format touched C++
and web files, build affected targets, and run `git diff --check`. Use the relevant renderer, transfer, display, and
CEF ingestion checks for structural changes. Hardware-sensitive changes require appropriate runtime coverage;
a successful build alone does not establish DMA, presentation, or shutdown correctness. No runtime validation is
claimed by this documentation-only change.
