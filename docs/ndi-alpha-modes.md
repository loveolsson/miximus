# NDI alpha modes

Both **NDI Input** and **NDI Output** have an **Alpha mode** dropdown:

| Mode | Input interpretation | Output representation |
| --- | --- | --- |
| Ignore alpha | Decode received RGB as opaque; discard the received alpha. | Recover unpremultiplied colors from the working image, encode them, and send alpha 1. |
| Straight alpha | Decode unassociated video RGB, then premultiply in linear light. | Unpremultiply linear working RGB, then encode; preserve alpha. |
| Premultiplied alpha | Unpremultiply received video RGB, decode, then premultiply in linear light. | Unpremultiply linear working RGB, encode, then premultiply in video space; preserve alpha. |
| Straight alpha over black | Decode straight RGB, premultiply in linear light, then force alpha to one. | Input only. |

The protocol option is `alpha_mode`, with values `ignore`, `straight`, and `premultiplied`. Both nodes default to
`straight`, including existing saved graphs without the option. Choose the mode to match the other application;
input and output choices are independent. Input additionally accepts `straight_over_black`. Settings take effect on subsequent evaluated frames without restarting
the NDI connection. Output frames already in the bounded queue keep their rendered representation.

Ignore on output recovers colors rather than flattening the image onto black. At zero alpha, premultiplied RGB has
no recoverable color: output is opaque black in Ignore mode and transparent black in the other modes. Input Ignore
still preserves received RGB even when its alpha byte is zero. NDI BGRX input remains opaque in every mode.

## Conversion and precision

Let `D` be Rec.709 decoding, `E` its encoding, `A` alpha, `V` received RGB, and `L` linear premultiplied working RGB.
RGB operations below are componentwise; division at zero alpha is replaced with zero.

| Mode | Input `(RGB, alpha)` | Output `(RGB, alpha)` |
| --- | --- | --- |
| Ignore | `(D(V), 1)` | `(E(L/A), 1)` |
| Straight alpha | `(D(V) × A, A)` | `(E(L/A), A)` |
| Premultiplied alpha | `(D(V/A) × A, A)` | `(E(L/A) × A, A)` |
| Straight alpha over black | `(D(V) × A, 1)` | Input only |

Straight alpha over black preserves the RGB produced by Straight alpha but makes the result opaque. Fully
transparent input becomes opaque black. Ignore alpha remains useful when the received RGB already contains the
desired appearance, including a premultiplied source viewed over black. Because transfer decoding is nonlinear,
compositing in linear light is not byte-equivalent to multiplying encoded RGB before decoding.

Conversion runs on the GPU into the existing four-channel UNORM16 linear working storage. Output converts before
the final RGBA8 quantization. Alpha is never gamma-corrected. This avoids applying nonlinear gamma directly to
premultiplied RGB, which would distort partially transparent colors. Unpremultiplication cannot recover precision
already lost when another application quantized very low-alpha premultiplied video.

## Verification

The GPU tests check all three receive modes against independent Rec.709 references across all 256 channel values
and seven alpha values (including zero and 1/255), using BGRA input and RGBA output. The over-black test checks the same channel and alpha range against a linear-light reference, requires fully opaque
output, and checks that BGRX stays opaque. A separate output test starts
from known UNORM16 premultiplied pixels and checks each mode independently, including opaque recovered colors in
Ignore mode and zero-alpha handling. Native option tests verify both node defaults, accepted values, and rejection
of unknown strings, numbers and booleans, plus acceptance of `straight_over_black` only on input.

On 2026-09-09, the native and web builds, 105 ordinary tests, and 23 GPU renderer tests passed. The GPU tests ran
with Vulkan synchronization validation on the Quadro P2000. Two 30-second SDK runs, using Vulkan staging and CUDA,
switched through all three matching modes and two independently selected input/output combinations. Input capture
and output send counters continued advancing, each NDI connection started only once, and old settings loaded with
the straight default. Original user settings were unchanged. The Vulkan run logged one unrelated HTTP WebSocket
EOF; neither run reported NDI or GPU validation errors.

These SDK runs verify live selection and frame flow; pixel accuracy is checked by the independent GPU tests rather
than inferred from a compressed NDI loopback.

On 2026-09-25, the input-only `straight_over_black` mode passed the native and web builds, 158 ordinary tests,
33 GPU renderer tests, and 14 GPU transfer tests. Both GPU suites ran with Vulkan synchronization validation.
An isolated NDI SDK loopback with private settings received straight-alpha output and switched through all four
input modes, returning to `straight_over_black`; receive and downstream send counters continued advancing.
Pixel accuracy for the new mode was verified separately by the independent GPU reference test above.
