#version 330 core
out vec4 FragColor;

in vec2 TexCoord; // the input variable from the vertex shader (same name and same type)

uniform sampler2D tex;

void main() { FragColor = texture(tex, TexCoord); }
// void main() { FragColor = vec4(1.0, 0, 0, 1.0); }