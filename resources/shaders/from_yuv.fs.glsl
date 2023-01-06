out vec4 FragColor;

in vec2 TexCoord; // the input variable from the vertex shader (same name and same type)

uniform sampler2D tex;
uniform int       target_width;
uniform mat3      transfer;

vec3 yuv2rgb(float Y, float Cb, float Cr)
{
    // float r, g, b;

    // Y: Undo 1/256 texture value scaling and scale [16..235] to [0..1] range
    // C: Undo 1/256 texture value scaling and scale [16..240] to [-0.5 .. + 0.5] range
    // Y  = (Y * 256.0 - 16.0) / 219.0;
    // Cb = (Cb * 256.0 - 16.0) / 224.0 - 0.5;
    // Cr = (Cr * 256.0 - 16.0) / 224.0 - 0.5;
    // Y  = (Y * 1024.0 - 64.0) / 876.0;
    // Cb = (Cb * 1024.0 - 64.0) / 896.0 - 0.5;
    // Cr = (Cr * 1024.0 - 64.0) / 896.0 - 0.5;

    // Convert to RGB using Rec.709 conversion matrix (see eq 26.7 in Poynton 2003)
    // r = Y + 1.5748 * Cr;
    // g = Y - 0.1873 * Cb - 0.4681 * Cr;
    // b = Y + 1.8556 * Cb;
    // return vec3(r, g, b);

    return vec3(Y, Cr - 0.5, Cb - 0.5) * transfer;
}

void main(void)
{
    vec2 tc = TexCoord;

    int x = int(tc.x * float(target_width));
    int y = int(textureSize(tex, 0).y * tc.y);

    float Y, Cb, Cr;

    int  start_x = x / 6 * 4;
    vec4 tex_1, tex_2;

    switch (x % 6) {
        case 0:
            tex_1 = texelFetch(tex, ivec2(start_x, y), 0);
            Y     = tex_1.y;
            Cb    = tex_1.x;
            Cr    = tex_1.z;
            break;
        case 1:
            tex_1 = texelFetch(tex, ivec2(start_x, y), 0);
            tex_2 = texelFetch(tex, ivec2(start_x + 1, y), 0);
            Y     = tex_2.x;
            Cb    = tex_1.x;
            Cr    = tex_1.z;
            break;
        case 2:
            tex_1 = texelFetch(tex, ivec2(start_x + 1, y), 0);
            tex_2 = texelFetch(tex, ivec2(start_x + 2, y), 0);
            Y     = tex_1.z;
            Cb    = tex_1.y;
            Cr    = tex_2.x;
            break;
        case 3:
            tex_1 = texelFetch(tex, ivec2(start_x + 1, y), 0);
            tex_2 = texelFetch(tex, ivec2(start_x + 2, y), 0);
            Y     = tex_2.y;
            Cb    = tex_1.y;
            Cr    = tex_2.x;
            break;
        case 4:
            tex_1 = texelFetch(tex, ivec2(start_x + 2, y), 0);
            tex_2 = texelFetch(tex, ivec2(start_x + 3, y), 0);
            Y     = tex_2.x;
            Cb    = tex_1.z;
            Cr    = tex_2.y;
            break;
        default:
            tex_1 = texelFetch(tex, ivec2(start_x + 2, y), 0);
            tex_2 = texelFetch(tex, ivec2(start_x + 3, y), 0);
            Y     = tex_2.z;
            Cb    = tex_1.z;
            Cr    = tex_2.y;
            break;
    }

    vec3 sRGB = yuv2rgb(Y, Cb, Cr);
    FragColor = vec4(toLinear(sRGB), 1.0);
}