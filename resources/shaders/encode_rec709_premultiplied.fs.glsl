out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D tex;
uniform int       readback_component_mapping;

void main()
{
    vec4 color = texture(tex, TexCoord);
    if (color.a <= 0.0) {
        FragColor = vec4(0.0);
        return;
    }

    vec4 encoded = vec4(from_linear(color.rgb / color.a) * color.a, color.a);
    FragColor    = map_readback_components(encoded, readback_component_mapping);
}
