#version 450

layout(location = 0) out vec2 uv;

// Shared prefix of the draw and mix push constants; rectangles store x/y/width/height.
layout(push_constant) uniform Parameters
{
    vec4  destination_rectangle;
    vec4  source_rectangle;
    float opacity;
    float target_width;
    float target_height;
    float mix_fraction;
} parameters;

const vec2 corners[6] =
    vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main()
{
    vec2 corner            = corners[gl_VertexIndex];
    vec2 pixel_position    = parameters.destination_rectangle.xy + corner * parameters.destination_rectangle.zw;
    vec2 target_dimensions = vec2(parameters.target_width, parameters.target_height);

    gl_Position = vec4(pixel_position / target_dimensions * 2.0 - 1.0, 0.0, 1.0);
    uv          = parameters.source_rectangle.xy + corner * parameters.source_rectangle.zw;
}
