# Vulkan behavior review

Comparison: the working tree against OpenGL commit `0047a63`. NDI alpha interpretation is excluded. A Vulkan port
does not authorize changes to source alignment, presentation policy, buffering, or failure recovery. The reasons below
describe what the implementation needs now; they do not establish that the old application was wrong.

## Restored in this follow-up

| Change | Restoration |
| --- | --- |
| Screen latency stopped following newly presented frames. | Each newly selected frame contributes its PTS and presentation-completion time to the original rolling latency estimate. Repeated frames do not contribute another observation. |
| NDI output stopped observing newly selected frame PTS. | Restore the original observation against the absolute send deadline on every new frame. |
| DeckLink correction required a full queue. | Restore its original timestamp/tolerance condition, independent of occupancy. Hardware-clock and completed-slot observations remain continuous. |
| Screen's remaining fallback correction required a full queue. | Preserve this original OpenGL fallback as explicitly confirmed by the user. Restore the separate continuous new-frame observations removed by the rewrite; those do not require any queue depth. Startup preroll also remains unchanged. |
| Mipmap generation moved from producers to the first minifying consumer. | Generate at the original upload, input-conversion, explicit framebuffer-to-texture, and implicit framebuffer-to-texture boundaries. This includes CUDA uploads. Keep defensive generation for a dirty image used directly through the GPU API. No measured benefit justified moving normal producer work to consumers. |
| Screen startup failures gained a one-second retry; stopped presenters restarted automatically. | Restore the previous node lifecycle. Configuration/dimension changes still recreate the presenter. Normal Vulkan swapchain recreation remains inside the presenter. |
| Transfer exceptions permanently consumed slots and set `allocation_failed`. | Quarantine the failed slot, wait for its external leases and frame uses, destroy its backend on the worker, release its memory budget, and allocate a replacement. A transfer failure is distinct from allocation failure. Device loss still cannot be repaired by replacing a slot. |
| Five-second GPU/WSI/CUDA retirement deadlines introduced earlier failure/termination. | Restore the old one-hour fence-wait allowance for these paths. Ordinary presentation dependency waits remain cancellable; the application's existing shutdown progress watchdog remains active. Unsafe destruction after an actual retirement error still cannot be allowed. |
| `swaps_completed` counted queue-present return rather than the old completion boundary. | Count completion callbacks after the GPU copy has completed. This still does not claim physical scanout. |
| A presenter reported stopped before GPU/WSI retirement, leaving that wait to destruction on the render thread. | Retire on the presenter worker before publishing stopped. The existing asynchronous stop/recreate path then waits through polling, without making render-thread replacement wait on GPU/WSI use. |
| Readback duration started at publication rather than worker processing. | Restore the timer start at the worker's first processing attempt, including dependency/copy waits but excluding preceding task-queue residence. |
| Float/integer test image formats became mandatory for starting the application. | Only application RGBA8 and RGBA16 UNORM formats gate the image-format floor. Optional graphics pipelines are created only where the required attachment capability exists. |

The screen nominal-clock policy, GLFW window ownership, saved window geometry, and short metadata-lock behavior had
already been restored in the preceding follow-up. That follow-up did not resolve the reported visual stutter.

## Upload readiness: unchanged pending the user's decision

There are three separate milestones:

1. The producer has finished writing the host buffer. The upload must not read a buffer still being written by the
   CPU or DeckLink DMA.
2. The upload has been submitted and a usable GPU dependency exists. The render CPU can record dependent drawing;
   GPU execution must wait before sampling the uploaded pixels.
3. The GPU transfer has completed. Host memory can be reused once every external lease has also ended. Readbacks
   additionally require host visibility/invalidation before an SDK may read them.

The persistent OpenGL backend's upload `wait_for_transfer_completion()` actually inserted `glWaitSync` on the worker
GPU context. Its upload worker then generated mipmaps and published an upload-ready fence. The render-side wait for
the exact upload ID therefore waited for this preparation/submission, followed by another GPU-side fence wait. The
CUDA backend could wait for actual CUDA completion on its transfer worker.

The Vulkan backend currently publishes the upload ID as ready only when its completion token reports GPU completion.
Consequently, the same render-side `wait_for_upload(id)` now waits for actual GPU completion. Buffered FIFO selection
often makes that wait cheap, but does not restore the old CPU/GPU distinction.

This stronger render-CPU wait is not required by Vulkan. It simplified ownership handling in the rewrite, which is
not a sufficient reason to change the contract. The equivalent design would publish submission readiness and its GPU
dependency separately from completion/reclamation, retaining the exact selected upload and host leases. The change
must cover both Vulkan and CUDA ownership handoffs. It must never use readiness to choose a different FIFO frame.
This follow-up leaves those readiness semantics unchanged as requested.

Vulkan supports execution and memory dependencies without making the host wait for completion; barriers and
semaphores supply the GPU ordering, while fences or host timeline waits serve host access requirements. See the
[synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).

## Retained implementation differences and their reasons

**Independent recordings and one graphics submission worker.** Each graph/transfer/presenter context owns its own
command pools. The worker serializes native queue access and resolves initial image layouts in actual submission
order. This allows another thread to finish recording without holding the graph recorder or its mutex. A global
recording lock would violate the requested thread-isolation contract. Vulkan command-pool and queue synchronization
requirements explain the ownership; they do not require this particular worker design. See the
[command-buffer specification](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html).

The single graphics queue is an implementation choice with a concrete purpose: shared images have one ordered layout
and memory-dependency history. Adding transfer queues requires explicit cross-queue ownership/dependencies and is
not equivalent to adding more command buffers. Recordings already run independently. This is not a claim that one
graphics queue is fastest. On hardware with only one suitable queue, a blocking presentation call can still delay
submission despite the render CPU never taking that queue lock.

**Bounded in-flight recordings and recoverable frame abandonment.** Vulkan cannot reuse a pending command pool or
its descriptors. Reusing them early corrupts outstanding work; waiting on the render thread violates the requested
nonblocking producer/consumer isolation; unbounded accumulation under a stalled GPU consumes unbounded memory.
Therefore an exhausted render context abandons the unfinished evaluation and retains the last published output.
Descriptor pages grow with draw demand, so there is no fixed draw count. The current 32-recording graph capacity is
a policy value, not a Vulkan limit or a measured optimum. It counts outstanding batches, including intermediate input
flushes, rather than frames. This remains a material overload behavior difference from OpenGL's driver-managed
submission, justified by bounded memory and the explicit no-wait requirement, not by a claim that the old scheduler
was faulty.

**RAII frame publication after native submission.** A pending output must not escape with a dependency whose commands
may still be abandoned. The frame scope retains leases until execution succeeds, then attaches publication to native
submission success. Already submitted commands retain their resources if a later batch fails. This implements the
previously requested RAII ownership design. It also means a failed evaluation does not publish its earlier output
leases individually. Successful `complete()` remains the original CPU cleanup phase; it is not a GPU completion wait.

**Transfer-worker polling across pending tasks.** With asynchronous submission and retained SDK leases, one pending
operation must not prevent the worker from progressing unrelated streams or reclaiming their slots. A pass retries
older pending tasks before new arrivals, then waits once for the whole set. Copy completions still publish in submission
order. The old blocking backend and delayed-reclaim loop cannot simply be copied around these asynchronous states:
waiting once per pending task adds delay proportional to pending work. This is a worker-mechanics change to preserve
independent progress, not permission to alter media timestamps. The one-millisecond polling interval is still a
scheduling cost, not an exact timing source or a benchmark-proven optimum.

**Retirement must prove GPU, CUDA and presentation ownership has ended.** OpenGL hid native resource deletion and
presentation-semaphore retirement. Vulkan exposes them. GPU copy completion alone does not prove that presentation
has consumed its semaphore; the maintenance fence closes that separate lifetime. CUDA and future DVP must also keep
registrations and host addresses alive through outstanding DMA. This justifies deferred destruction and refusing unsafe
freeing, not permanent slot loss, shortened arbitrary deadlines, or changing frame selection.

**Device selection and capability checks.** Vulkan needs an explicit device and its queue/features. Preferring a
hardware GPU over a software implementation avoids accidentally running a real-time mixer on the CPU; the existing
UUID override permits deterministic selection. Discrete-over-integrated preference is a default policy, not proof of
the fastest GPU. Vulkan 1.3/dynamic rendering/synchronization2 and the selected output/format features belong to the
implementation's actual requirements. Surface-specific presentation support is still checked when creating the
presenter. Automatic selection across mixed devices, particularly when only some support the requested output, remains
a compatibility limitation; removing the extra test-format floor does not establish compatibility on untested devices.

**Draw validation.** Sampling an image while writing that same attachment cannot simply reuse the ordinary Vulkan
draw path. Rejecting unsupported feedback, invalid handles and non-finite GPU parameters prevents invalid commands.
This is a safety reason for validation, not evidence that every graph accepted by OpenGL was wrong. Such API misuse
still throws; an uncaught draw exception can terminate the application. A graph that reaches this path needs a
specific compatibility fix or an explicit recoverable graph error, not removal of synchronization checks. No such
valid-graph reproduction was established by this review.

**Pipeline preparation before rendering.** Shader SPIR-V exists at build time, but the driver still creates native
pipelines. Doing that during device startup avoids introducing driver compilation into a live render frame. Measured
pipeline-creation costs are recorded in [the implementation status](vulkan-progress.md). The application disk cache
remains removed as requested. The source/bundler/submodule layout follows the project's established dependency rules.

**Remaining explicitly requested behavior.** CUDA remains selectable through `--use-cuda`, with no fallback when it
is explicitly required. Producers submit before putting frames into their timed FIFOs. DeckLink retains direct access
to backend-owned transfer memory and its external lease contract for future DVP. These are retained user instructions,
not independent reinterpretations of the migration plan.

**Construction and packing corrections.** Starting the configuration thread only after GPU/service construction
avoids destruction of a joinable `std::thread` during constructor unwinding. v210 tail packing avoids fetching pixels
outside the image and defines unused packed words. Those address concrete exception-lifetime and bounds problems;
neither is a reason to change timing policy or active-pixel color conventions.

## Validation

The normal build and all 110 deterministic tests passed. With Khronos synchronization validation on the Quadro P2000,
27 renderer tests, ten Vulkan transfer tests, ten CUDA transfer tests and five window tests passed. The mipmap
regression now explicitly generates on the producer recording, submits, samples from a separate recording, and checks
that abandoned writes/generation do not change the resulting image.

The dedicated `build-tidy` build also passed without diagnostics, including the final confirmed screen fallback
condition. An initial clang-tidy 21 process crashed in a Boost template matcher; the unchanged file passed on retry,
and subsequent complete builds passed with the same checks enabled.

A 30-second copied hardware-graph run injected one CUDA H2D submission error and one D2H submission error, without
altering the saved settings. They reached DeckLink input and NDI output respectively. Each failed backend allocation
was replaced and its replacement completed a transfer. Both streams continued: the final status showed one input
transfer failure and one output transfer failure, no allocation failure, and successful shutdown. Synchronization
validation reported no errors. This exercises recoverable submission failure, not device loss or a permanently stalled
DMA engine. The temporary interposer, runner and logs are under `/tmp/miximus-behavior-validation/`.

The ordinary Vulkan hardware graph also completed a 30-second run with synchronization validation and no reported
errors. The ordinary CUDA graph completed without the optional layer, and ASan/UBSan with leak detection passed that
same graph. All 110 deterministic tests also passed under ASan/UBSan. Existing CUDA and driver sanitizer settings were
preserved.

The packaged Khronos layer 1.4.341.0 crashed during two CUDA hardware
runs under its default locking, once in `BatchAccessLog::CBSubmitLog::GetAccessRecord` during a timeline query and once
inside queue submission in the sanitized run. No invalid-operation diagnostic preceded either crash. Symbolized core
and sanitizer traces are retained with the temporary logs. Both the ordinary and sanitized CUDA graph completed
30-second runs with all validation checks enabled and `VK_LAYER_FINE_GRAINED_LOCKING=false`, the layer's documented
diagnostic locking setting. Its startup performance warning is expected. That initial comparison alone did not
establish a root cause.

The subsequent crash investigation instrumented the old layer itself with ASan and reproduced a heap-use-after-free
inside its `BatchAccessLog`: presentation imported an entry while submission trimmed and freed it. This identifies
the layer's concurrent history tracking as the failing ownership, rather than merely attributing an unsymbolized
crash to whichever library appeared in the stack. The official SDK 1.4.357.0 layer includes the upstream fix and is
now the validation setup; see [Vulkan verification](vulkan-progress.md#build-and-verification). Application
synchronization, CUDA handoffs, upload readiness and sanitizer safeguards were not changed to mask the failure.
The new window regression exercises presentation concurrently with another context's submissions and resource
retirement. An ordinary successful run without validation is still not counted as a validation pass.

For an isolated confirmation, the old ASan-instrumented layer was rebuilt with only the upstream QueuePresent
mutex acquisition added (retaining its existing mutex name). The same CUDA hardware graph then completed 90 seconds
and shutdown with default locking, synchronization validation, ASan/UBSan and leak detection enabled. No application
binary or sanitizer default changed between the failing and corrected-layer runs. The diagnostic backport remains
outside the repository; the supported test setup uses the official SDK layer, not a project-maintained layer fork.

With the official SDK 1.4.357.0 layer and default locking, the normal build passed 27 renderer, ten Vulkan transfer,
ten CUDA transfer and six window tests. The normal and dedicated tidy builds passed, with no tidy diagnostics, as
did all 110 deterministic tests. The CUDA hardware graph completed both an initial 30-second run and a 120-second
run through shutdown. Its status snapshot showed continuing NDI/DeckLink readbacks and screen presentation, with
no transfer failures or validation errors. These are correctness checks, not transfer-speed or visual-cadence claims.
The official-layer CUDA hardware graph also completed 30-second and 90-second ASan/UBSan runs through shutdown,
with leak detection and the existing driver-specific suppressions enabled, and no unsuppressed diagnostics.
The sanitizer renderer and both transfer suites passed with the official layer too. All six sanitizer window-test
assertions passed without validation errors, but that process exited 1 for a 32-byte XCB/XGetWindowProperty leak
through GLFW event polling. This is the separately documented window-suite leak, not a passing LSan result; no
suppression was added for it.

The failing instrumented trace, isolated backport diff, SDK checksums, build/test logs and copied hardware-run logs
are retained under `/tmp/miximus-cuda-crash/`. Within the repository, SDK binaries live only in the ignored test-tool
directory documented above. No changes were staged.

Saved settings retained their original SHA-256. All changes from this follow-up remain unstaged.

Short status-counter runs are not evidence that the reported periodic visual stuttering has disappeared. That requires
visual/presentation validation beyond successful transfer completion and build checks.

## Subsequent approved screen timing correction

The desired-result review and instruction to correct its issues supersede the earlier restored nominal X11 policy and
raw selected-PTS latency feedback described above. Screen output now selects nearest PTS, uses a GPU producer wait,
and follows FIFO presentation-ID completion where supported. Present-wait notifications are labelled as estimates;
this machine exposes no Vulkan scanout timestamps. The clock handles delayed host notifications without accumulating
extra display intervals. Continuous scheduling-error feedback excludes source-frame quantization and slews phase
corrections, while completion-minus-selected-PTS remains the latency metric. The original full-queue fallback remains.

Validation for this correction uses the normal build: 125 CTests and six display tests pass, including a run of the
latter with Vulkan synchronization validation. Earlier tidy/sanitizer results apply to the previous implementation;
those builds are explicitly deferred until the user confirms this screen fix works visually.
