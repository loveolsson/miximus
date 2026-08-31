out vec4 FragColor;

in vec2 TexCoord; // the input variable from the vertex shader (same name and same type)

uniform sampler2D tex;
uniform int       output_component_mapping;

void main()
{
    vec4 color = texture(tex, TexCoord);
    FragColor = map_output_components(vec4(from_linear(color.xyz), color.w), output_component_mapping);
}
