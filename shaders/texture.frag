#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform Parameters
{
    vec4  destination_rectangle;
    vec4  source_rectangle;
    float opacity;
    float target_width;
    float target_height;
    float mix_fraction;
    int   color_operation;
    uint  input_order;
    uint  output_order;

    // Unused tail bytes preserve the matching 64-byte C++ push-constant layout.
    int memory_layout_padding;
} parameters;

void main()
{
    color = map_input_channels(texture(source, uv), parameters.input_order);

    // External alpha modes are interpreted in encoded RGB. Working RGB is linear and premultiplied.
    switch (parameters.color_operation) {
        case color_operation_decode_rec709:
            color.rgb = to_linear(color.rgb);
            break;

        case color_operation_encode_rec709:
            color.rgb = from_linear(color.rgb);
            break;

        case color_operation_encode_rec709_premultiplied:
            color = to_video_premultiplied(color);
            break;

        case color_operation_decode_rec709_straight_alpha:
            color = vec4(to_linear(color.rgb) * color.a, color.a);
            break;

        case color_operation_encode_rec709_straight_alpha:
            color = to_video_straight(color);
            break;

        case color_operation_decode_rec709_premultiplied:
            color = to_linear_premultiplied(color);
            break;

        case color_operation_decode_rec709_ignore_alpha:
            color = vec4(to_linear(color.rgb), 1.0);
            break;

        case color_operation_encode_rec709_ignore_alpha:
            color = encode_rec709_ignore_alpha(color);
            break;

        case color_operation_encode_srgb_premultiplied:
            color = linear_to_srgb_premultiplied(color);
            break;

        case color_operation_decode_srgb_premultiplied:
            color = srgb_to_linear_premultiplied(color);
            break;
    }

    color = map_output_channels(color, parameters.output_order);
    color *= parameters.opacity;
}
