#version 330 core
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;

uniform vec2 offset;
uniform vec2 scale;

out vec2 TexCoord; // specify a color output to the fragment shader

void main()
{
    vec2 p = pos * scale + offset;
    p *= vec2(2.0);
    p -= vec2(1.0);

    gl_Position = vec4(p, 0.0, 1.0); // see how we directly give a vec3 to vec4's constructor
    TexCoord    = uv;                // set the output variable to a dark-red color
}