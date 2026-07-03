# color_transfer.hpp — Review Notes

These are potential issues found during code review. Each needs to be confirmed
before any fix is applied.

---

## 1. Cb/Cr swap in `from_yuv` shader (likely bug)

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

**To confirm**: check actual output colours — wrong hues (reddish-blue swap) would confirm this.

---

## 2. Missing Y black-level offset in `from_yuv` shader

Limited-range luma is in `[16/255, 235/255]`. The correct full-range expansion is:

```
Y_full = (Y_limited − 16/255) × (255/219)
```

The matrix scale `255/219` is baked into the coefficients, but the constant term
`−(16/255 × 255/219) = −16/219 ≈ −0.073` is never added anywhere in the shader.

The shader only handles the chroma offset (`Cb − 0.5`, `Cr − 0.5`), not the luma
black level. This would cause a slight brightness offset (black lifting) in the output.

**To confirm**: feed a signal with known black level and measure the output.

---

## 3. Wrong scale direction in `get_color_transfer_to_yuv`

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

**To confirm**: record the to_yuv output and verify it stays within limited-range bounds.

---

## 4. Chroma uses luma range for scaling (minor inaccuracy)

The `scale` function uses the luma limited range (219 levels, `[16, 235]`) for all
channels. Chroma (Cb, Cr) has a different limited range (224 levels, `[16, 240]`).

| Channel | Correct scale | Used scale | Error |
|---------|--------------|------------|-------|
| Y       | `255/219 ≈ 1.1644` | `255/219` | — |
| Cb, Cr  | `255/224 ≈ 1.1384` | `255/219` | ≈ +2.3% |

This causes a slight chroma saturation error (~2.3%). May be negligible in practice.

---

## Summary table

| # | Severity | Description | Confidence |
|---|----------|-------------|------------|
| 1 | High | Cb/Cr swapped in `from_yuv` shader vs matrix column order | Medium — needs visual confirmation |
| 2 | Medium | Missing Y black-level offset (−16/219) in `from_yuv` | High |
| 3 | High | `to_yuv` matrix scales in wrong direction (×255/219 instead of ×219/255) | High |
| 4 | Low | Chroma uses luma range for scale (255/219 vs 255/224) | High |
