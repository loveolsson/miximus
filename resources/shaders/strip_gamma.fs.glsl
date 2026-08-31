out vec4 FragColor;

in vec2 TexCoord; // the input variable from the vertex shader (same name and same type)

uniform sampler2D tex;
uniform int       input_component_mapping;

void main()
{
    vec4 color = map_input_components(texture(tex, TexCoord), input_component_mapping);
    FragColor = vec4(to_linear(color.xyz), color.w);
}
