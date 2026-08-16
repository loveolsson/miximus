out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D tex;
uniform sampler2D tex_b;
uniform float     t;
uniform vec2      a_destination_offset;
uniform vec2      a_destination_scale;
uniform vec2      a_source_offset;
uniform vec2      a_source_scale;
uniform vec2      b_destination_offset;
uniform vec2      b_destination_scale;
uniform vec2      b_source_offset;
uniform vec2      b_source_scale;
uniform int       video_mix;

vec4 sample_fitted(sampler2D image, vec2 destination_offset, vec2 destination_scale, vec2 source_offset, vec2 source_scale)
{
    vec2 uv = (TexCoord - destination_offset) / destination_scale;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return vec4(0.0);
    }
    return texture(image, source_offset + uv * source_scale);
}

vec4 to_video_premultiplied(vec4 color)
{
    if (color.a <= 0.0) {
        return vec4(0.0);
    }
    return vec4(from_linear(color.rgb / color.a) * color.a, color.a);
}

vec4 to_linear_premultiplied(vec4 color)
{
    if (color.a <= 0.0) {
        return vec4(0.0);
    }
    return vec4(to_linear(color.rgb / color.a) * color.a, color.a);
}

void main()
{
    if (t <= 0.0) {
        FragColor = sample_fitted(
            tex, a_destination_offset, a_destination_scale, a_source_offset, a_source_scale);
        return;
    }
    if (t >= 1.0) {
        FragColor = sample_fitted(
            tex_b, b_destination_offset, b_destination_scale, b_source_offset, b_source_scale);
        return;
    }

    vec4 a = sample_fitted(tex, a_destination_offset, a_destination_scale, a_source_offset, a_source_scale);
    vec4 b = sample_fitted(tex_b, b_destination_offset, b_destination_scale, b_source_offset, b_source_scale);

    if (video_mix != 0) {
        FragColor = to_linear_premultiplied(mix(to_video_premultiplied(a), to_video_premultiplied(b), t));
    } else {
        FragColor = mix(a, b, t);
    }
}
