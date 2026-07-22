out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D tex;
uniform sampler2D tex_b;
uniform float     t;
uniform vec2      a_offset;
uniform vec2      a_scale;
uniform vec2      b_offset;
uniform vec2      b_scale;
uniform int       video_mix;

vec4 sample_contained(sampler2D image, vec2 offset, vec2 scale)
{
    vec2 uv = (TexCoord - offset) / scale;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return vec4(0.0);
    }
    return texture(image, uv);
}

vec4 to_video_premultiplied(vec4 color)
{
    if (color.a <= 0.0) {
        return vec4(0.0);
    }
    return vec4(fromLinear(color.rgb / color.a) * color.a, color.a);
}

vec4 to_linear_premultiplied(vec4 color)
{
    if (color.a <= 0.0) {
        return vec4(0.0);
    }
    return vec4(toLinear(color.rgb / color.a) * color.a, color.a);
}

void main()
{
    if (t <= 0.0) {
        FragColor = sample_contained(tex, a_offset, a_scale);
        return;
    }
    if (t >= 1.0) {
        FragColor = sample_contained(tex_b, b_offset, b_scale);
        return;
    }

    vec4 a = sample_contained(tex, a_offset, a_scale);
    vec4 b = sample_contained(tex_b, b_offset, b_scale);

    if (video_mix != 0) {
        FragColor = to_linear_premultiplied(mix(to_video_premultiplied(a), to_video_premultiplied(b), t));
    } else {
        FragColor = mix(a, b, t);
    }
}
