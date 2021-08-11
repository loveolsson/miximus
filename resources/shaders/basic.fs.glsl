out vec4 FragColor;

in vec2 TexCoord; // the input variable from the vertex shader (same name and same type)

uniform sampler2D tex;
uniform float     opacity = 1.0;

void main() { FragColor = texture(tex, TexCoord) * opacity; }
