#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D source_a;
layout(set = 0, binding = 1) uniform sampler2D source_b;

layout(push_constant) uniform Parameters
{
    vec4  destination_rectangle;
    vec4  source_rectangle;
    float opacity;
    float target_width;
    float target_height;
    float mix_fraction;
    vec4  a_destination;
    vec4  a_source;
    vec4  b_destination;
    vec4  b_source;
    int   video_mix;

    // Unused tail bytes preserve the matching 128-byte C++ push-constant layout.
    int memory_layout_padding[3];
} parameters;

vec4 sample_fitted(sampler2D image, vec4 destination_rectangle, vec4 source_rectangle)
{
    if (any(equal(destination_rectangle.zw, vec2(0.0)))) {
        return vec4(0.0);
    }

    vec2 source_uv = (uv - destination_rectangle.xy) / destination_rectangle.zw;
    // Letterbox/pillarbox borders outside the fitted image are transparent black.
    if (any(lessThan(source_uv, vec2(0.0))) || any(greaterThan(source_uv, vec2(1.0)))) {
        return vec4(0.0);
    }

    return texture(image, source_rectangle.xy + source_uv * source_rectangle.zw);
}

void main()
{
    vec4 color_a = sample_fitted(source_a, parameters.a_destination, parameters.a_source);
    vec4 color_b = sample_fitted(source_b, parameters.b_destination, parameters.b_source);

    if (parameters.mix_fraction <= 0.0) {
        color = color_a;
    } else if (parameters.mix_fraction >= 1.0) {
        color = color_b;
    } else if (parameters.video_mix != 0) {
        vec4 video_mix = mix(to_video_premultiplied(color_a), to_video_premultiplied(color_b), parameters.mix_fraction);
        color          = to_linear_premultiplied(video_mix);
    } else {
        color = mix(color_a, color_b, parameters.mix_fraction);
    }
}
