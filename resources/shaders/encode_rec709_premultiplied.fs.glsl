out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D tex;

void main()
{
    vec4 color = texture(tex, TexCoord);
    if (color.a <= 0.0) {
        FragColor = vec4(0.0);
        return;
    }

    FragColor = vec4(fromLinear(color.rgb / color.a) * color.a, color.a);
}
