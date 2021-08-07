#version 330 core
out vec4 FragColor;

in vec2 TexCoord; // the input variable from the vertex shader (same name and same type)

uniform sampler2D tex;
// UYVY macropixel texture passed as RGBA format

vec4 rec709YCbCr2rgba(float Y, float Cb, float Cr, float a)
{
    float r, g, b;

    // Y: Undo 1/256 texture value scaling and scale [16..235] to [0..1] range
    // C: Undo 1/256 texture value scaling and scale [16..240] to [-0.5 .. + 0.5] range
    Y  = (Y * 256.0 - 16.0) / 219.0;
    Cb = (Cb * 256.0 - 16.0) / 224.0 - 0.5;
    Cr = (Cr * 256.0 - 16.0) / 224.0 - 0.5;

    // Convert to RGB using Rec.709 conversion matrix (see eq 26.7 in Poynton 2003)
    r = Y + 1.5748 * Cr;
    g = Y - 0.1873 * Cb - 0.4681 * Cr;
    b = Y + 1.8556 * Cb;
    return vec4(r, g, b, a);
}

// Perform bilinear interpolation between the provided components.
// The samples are expected as shown:
// ---------
// | X | Y |
// |---+---|
// | W | Z |
// ---------
vec4 bilinear(vec4 W, vec4 X, vec4 Y, vec4 Z, vec2 weight)
{
    vec4 m0 = mix(W, Z, weight.x);
    vec4 m1 = mix(X, Y, weight.x);
    return mix(m0, m1, weight.y);
}

// Gather neighboring YUV macropixels from the given texture coordinate
void textureGatherYUV(sampler2D UYVYsampler, vec2 tc, out vec4 W, out vec4 X, out vec4 Y, out vec4 Z)
{
    ivec2 tx   = ivec2(tc * textureSize(UYVYsampler, 0));
    ivec2 tmin = ivec2(0, 0);
    ivec2 tmax = textureSize(UYVYsampler, 0) - ivec2(1, 1);
    W          = texelFetch(UYVYsampler, tx, 0);
    X          = texelFetch(UYVYsampler, clamp(tx + ivec2(0, 1), tmin, tmax), 0);
    Y          = texelFetch(UYVYsampler, clamp(tx + ivec2(1, 1), tmin, tmax), 0);
    Z          = texelFetch(UYVYsampler, clamp(tx + ivec2(1, 0), tmin, tmax), 0);
}

void main(void)
{
    /* The shader uses texelFetch to obtain the YUV macropixels to avoid unwanted interpolation
     * introduced by the GPU interpreting the YUV data as RGBA pixels.
     * The YUV macropixels are converted into individual RGB pixels and bilinear interpolation is applied. */
    vec2  tc    = TexCoord;
    float alpha = 0.7;

    vec4 macro, macro_u, macro_r, macro_ur;
    vec4 pixel, pixel_r, pixel_u, pixel_ur;
    textureGatherYUV(tex, tc, macro, macro_u, macro_ur, macro_r);

    //   Select the components for the bilinear interpolation based on the texture coordinate
    //   location within the YUV macropixel:
    //   -----------------          ----------------------
    //   | UY/VY | UY/VY |          | macro_u | macro_ur |
    //   |-------|-------|    =>    |---------|----------|
    //   | UY/VY | UY/VY |          | macro   | macro_r  |
    //   |-------|-------|          ----------------------
    //   | RG/BA | RG/BA |
    //   -----------------
    vec2 off = fract(tc * textureSize(tex, 0));
    if (off.x > 0.5) {
        // right half of macropixel
        pixel    = rec709YCbCr2rgba(macro.a, macro.b, macro.r, alpha);
        pixel_r  = rec709YCbCr2rgba(macro_r.g, macro_r.b, macro_r.r, alpha);
        pixel_u  = rec709YCbCr2rgba(macro_u.a, macro_u.b, macro_u.r, alpha);
        pixel_ur = rec709YCbCr2rgba(macro_ur.g, macro_ur.b, macro_ur.r, alpha);

    } else {
        // left half & center of macropixel
        pixel    = rec709YCbCr2rgba(macro.g, macro.b, macro.r, alpha);
        pixel_r  = rec709YCbCr2rgba(macro.a, macro.b, macro.r, alpha);
        pixel_u  = rec709YCbCr2rgba(macro_u.g, macro_u.b, macro_u.r, alpha);
        pixel_ur = rec709YCbCr2rgba(macro_u.a, macro_u.b, macro_u.r, alpha);
    }

    FragColor = bilinear(pixel, pixel_u, pixel_ur, pixel_r, off);
}