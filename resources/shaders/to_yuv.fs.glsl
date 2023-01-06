out vec4 FragColor;

in vec2 TexCoord; // the input variable from the vertex shader (same name and same type)

uniform sampler2D tex;
uniform int       target_width;
uniform mat3      transfer;

vec3 rgb2yuv(vec3 rgb)
{
    return (transfer * fromLinear(rgb)) + vec3(0, 0.5, 0.5);

    // float Y, Cb, Cr;
    // r = Y + 1.5748 * Cr;
    // g = Y - 0.1873 * Cb - 0.4681 * Cr;
    // b = Y + 1.8556 * Cb;

    // Y  = (Y * 1024.0 - 64.0) / 876.0;
    // Cb = (Cb * 1024.0 - 64.0) / 896.0 - 0.5;
    // Cr = (Cr * 1024.0 - 64.0) / 896.0 - 0.5;

    // Convert to RGB using Rec.709 conversion matrix (see eq 26.7 in Poynton 2003)

    // return vec3(Y, Cb, Cr);

    //    return vec4(transfer * vec3(Y, Cb, Cr), a);
}

void main(void)
{
    vec2 tc = TexCoord;

    ivec2 size = textureSize(tex, 0);
    int   x    = int(tc.x * target_width);
    int   y    = int(tc.y * size.y);

    vec3  tex_1, tex_2, tex_3, tex_4;
    float X, Y, Z;

    int start_x = x / 4 * 6;

    switch (x % 4) {
        case 0:
            tex_1 = rgb2yuv(texelFetch(tex, ivec2(start_x, y), 0).xyz);
            tex_2 = rgb2yuv(texelFetch(tex, ivec2(start_x + 1, y), 0).xyz);
            X     = (tex_1.y + tex_2.y) / 2.0;
            Y     = tex_1.x;
            Z     = (tex_1.z + tex_2.z) / 2.0;
            break;
        case 1:
            tex_1 = rgb2yuv(texelFetch(tex, ivec2(start_x + 1, y), 0).xyz);
            tex_2 = rgb2yuv(texelFetch(tex, ivec2(start_x + 2, y), 0).xyz);
            tex_3 = rgb2yuv(texelFetch(tex, ivec2(start_x + 3, y), 0).xyz);
            X     = tex_1.x;
            Y     = (tex_2.y + tex_3.y) / 2.0;
            Z     = tex_2.x;
            break;
        case 2:
            tex_1 = rgb2yuv(texelFetch(tex, ivec2(start_x + 2, y), 0).xyz);
            tex_2 = rgb2yuv(texelFetch(tex, ivec2(start_x + 3, y), 0).xyz);
            tex_3 = rgb2yuv(texelFetch(tex, ivec2(start_x + 4, y), 0).xyz);
            tex_4 = rgb2yuv(texelFetch(tex, ivec2(start_x + 5, y), 0).xyz);
            X     = (tex_1.z + tex_2.z) / 2.0;
            Y     = tex_2.x;
            Z     = (tex_3.y + tex_4.y) / 2.0;
            break;
        default:
            tex_1 = rgb2yuv(texelFetch(tex, ivec2(start_x + 4, y), 0).xyz);
            tex_2 = rgb2yuv(texelFetch(tex, ivec2(start_x + 5, y), 0).xyz);
            X     = tex_1.x;
            Y     = (tex_1.z + tex_2.z) / 2.0;
            Z     = tex_2.x;
            break;
    }

    FragColor = vec4(vec3(X, Y, Z), 0.0);
}