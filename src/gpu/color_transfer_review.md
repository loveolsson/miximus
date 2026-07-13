# color_transfer.hpp — Review Notes

These issues were confirmed against the shader matrix orientation, the normalized
OpenGL texture representation, and ITU-R BT.709-6. They have been corrected.

---

## 1. Cb/Cr swap in `from_yuv` shader (confirmed)

The matrix in `get_color_transfer_from_yuv` is built for input order **(Y, Cb, Cr)**:
- Column 1 holds the standard **Cb** coefficients (B-channel gets `(1−Wb)/0.5`, R-channel gets `0`)
- Column 2 holds the standard **Cr** coefficients (R-channel gets `(1−Wr)/0.5`, B-channel gets `0`)

But `from_yuv.fs.glsl` calls:
```glsl
return vec3(Y, Cr - 0.5, Cb - 0.5) * transfer;
```

In a GLSL `vec * mat` multiplication, column 1 is applied to the second component
(`Cr` here) and column 2 to the third (`Cb`). This means:
- **R** ends up using the `(1−Wr)/0.5` coefficient, but applied to **Cb** instead of **Cr**
- **B** ends up using the `(1−Wb)/0.5` coefficient, but applied to **Cr** instead of **Cb**

The fix would be either:
- Change the shader to `vec3(Y, Cb - 0.5, Cr - 0.5)`, **or**
- Swap columns 1 and 2 in the matrix.

**Resolution**: the shader now passes `(Y, Cb, Cr)` to the inverse matrix.

---

## 2. Missing Y black-level offset in `from_yuv` shader (confirmed)

Limited-range luma is in `[16/255, 235/255]`. The correct full-range expansion is:

```
Y_full = (Y_limited − 16/255) × (255/219)
```

The matrix scale `255/219` is baked into the coefficients, but the constant term
`−(16/255 × 255/219) = −16/219 ≈ −0.073` is never added anywhere in the shader.

The shader only handles the chroma offset (`Cb − 0.5`, `Cr − 0.5`), not the luma
black level. This would cause a slight brightness offset (black lifting) in the output.

**Resolution**: decode now subtracts the complete normalized 10-bit offset
`(64, 512, 512) / 1023` before applying separate luma and chroma expansion.

---

## 3. Wrong scale direction in `get_color_transfer_to_yuv` (confirmed)

Both `get_color_transfer_from_yuv` and `get_color_transfer_to_yuv` use the same
`scale` lambda:

```cpp
return (float)(1.0 * max / (white - black) * v);  // = 255/219 × v ≈ 1.164 × v
```

- For **from_yuv** this is correct: it *expands* limited-range to full-range.
- For **to_yuv** this is the wrong direction: it *further expands*, giving Y values
  up to `≈ 1.164` rather than compressing into `[16/255, 235/255]`.

The to_yuv scale should be `219/255` for luma (and `224/255` for chroma), and the
black-level offset `+16/255` for Y should be added (currently only `+0.5` is added
for chroma channels, in the shader).

**Resolution**: encode now compresses luma by `876/1023`, compresses chroma by
`896/1023`, and adds `(64, 512, 512) / 1023`.

---

## 4. Chroma uses luma range for scaling (confirmed)

The `scale` function uses the luma limited range (219 levels, `[16, 235]`) for all
channels. Chroma (Cb, Cr) has a different limited range (224 levels, `[16, 240]`).

| Channel | Correct scale | Used scale | Error |
|---------|--------------|------------|-------|
| Y       | `255/219 ≈ 1.1644` | `255/219` | — |
| Cb, Cr  | `255/224 ≈ 1.1384` | `255/219` | ≈ +2.3% |

This causes a slight chroma saturation error (~2.3%). May be negligible in practice.

**Resolution**: luma and chroma use independent scales in both directions.

---

## Additional normalization detail

The DeckLink path uses `GL_RGB10_A2`, whose unsigned normalized 10-bit channels
map code values through `[0, 1023]` to `[0, 1]`. BT.709's nominal 10-bit ranges
are Y `[64, 940]`, Cb/Cr `[64, 960]`, with chroma zero at 512. The conversion
therefore uses those 10-bit values directly rather than 8-bit approximations.

## Display-mode colorimetry

DeckLink display modes advertise supported Rec.601, Rec.709, and Rec.2020
colorimetry. Output mode records now cache the corresponding YCbCr conversion:

- SD prefers Rec.601.
- HD prefers Rec.709.
- UHD prefers Rec.2020 when advertised by the mode, with a resolution-based
  fallback for modes that do not expose colorimetry flags.

Rec.2020 output first converts the project's linear Rec.709 primaries into
linear Rec.2020 primaries, then applies the SDR transfer function and Rec.2020
YCbCr matrix. Frames are tagged with the matching DeckLink colorspace metadata.

DeckLink input performs the inverse operation. Per-frame colorspace metadata is
preferred when available, with detected display-mode colorimetry as fallback.
Rec.2020 YCbCr is decoded into linear Rec.2020 and then converted into the
project's linear Rec.709 working primaries.

---

## Summary table

| # | Severity | Description | Status |
|---|----------|-------------|------------|
| 1 | High | Cb/Cr swapped in `from_yuv` shader vs matrix column order | Confirmed and fixed |
| 2 | Medium | Missing Y black-level offset | Confirmed and fixed |
| 3 | High | `to_yuv` matrix scales in wrong direction | Confirmed and fixed |
| 4 | Low | Chroma uses luma range for scaling | Confirmed and fixed |

## References

- [ITU-R BT.709-6](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I!!PDF-E.pdf),
  particularly the 8-bit and 10-bit quantization levels in section 4.
- [Khronos Data Format Specification](https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.html),
  for unsigned normalized integer mapping to `[0, 1]`.
