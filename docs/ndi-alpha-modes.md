# NDI alpha modes

Both **NDI Input** and **NDI Output** have an **Alpha mode** dropdown:

| Mode | Input interpretation | Output representation |
| --- | --- | --- |
| Ignore alpha | Decode received RGB as opaque; discard the received alpha. | Recover unpremultiplied colors from the working image, encode them, and send alpha 1. |
| Straight | Decode unassociated video RGB, then premultiply in linear light. | Unpremultiply linear working RGB, then encode; preserve alpha. |
| Premultiplied | Unpremultiply received video RGB, decode, then premultiply in linear light. | Unpremultiply linear working RGB, encode, then premultiply in video space; preserve alpha. |

The protocol option is `alpha_mode`, with values `ignore`, `straight`, and `premultiplied`. Both nodes default to
`straight`, including existing saved graphs without the option. Choose the mode to match the other application;
input and output choices are independent. Settings take effect on subsequent evaluated frames without restarting
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
| Straight | `(D(V) × A, A)` | `(E(L/A), A)` |
| Premultiplied | `(D(V/A) × A, A)` | `(E(L/A) × A, A)` |

Conversion runs on the GPU into the existing four-channel UNORM16 linear working storage. Output converts before
the final RGBA8 quantization. Alpha is never gamma-corrected. This avoids applying nonlinear gamma directly to
premultiplied RGB, which would distort partially transparent colors. Unpremultiplication cannot recover precision
already lost when another application quantized very low-alpha premultiplied video.

## Verification

The GPU tests check all three receive modes against independent Rec.709 references across all 256 channel values
and seven alpha values (including zero and 1/255), using BGRA input and RGBA output. A separate output test starts
from known UNORM16 premultiplied pixels and checks each mode independently, including opaque recovered colors in
Ignore mode and zero-alpha handling. Native option tests verify both node defaults, accepted values, and rejection
of unknown strings, numbers and booleans.

On 2026-09-09, the native and web builds, 105 ordinary tests, and 23 GPU renderer tests passed. The GPU tests ran
with Vulkan synchronization validation on the Quadro P2000. Two 30-second SDK runs, using Vulkan staging and CUDA,
switched through all three matching modes and two independently selected input/output combinations. Input capture
and output send counters continued advancing, each NDI connection started only once, and old settings loaded with
the straight default. Original user settings were unchanged. The Vulkan run logged one unrelated HTTP WebSocket
EOF; neither run reported NDI or GPU validation errors.

These SDK runs verify live selection and frame flow; pixel accuracy is checked by the independent GPU tests rather
than inferred from a compressed NDI loopback.
