out vec4 FragColor;

in vec2 TexCoord; // the input variable from the vertex shader (same name and same type)

uniform sampler2D tex;

void main()
{
    vec4 color = texture(tex, TexCoord);
    FragColor  = vec4(toLinear(color.xyz), color.w);
}
