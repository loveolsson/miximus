// Wikipedia https://en.wikipedia.org/wiki/Rec._709
const float gamma_offset     = 0.099;
const float gamma_gamma      = 0.45;
const float gamma_cutoffTo   = 0.081;
const float gamma_cutoffFrom = 0.018;
const float gamma_div        = 4.5;

// sRGB
// const float gamma_offset     = 0.055;
// const float gamma_gamma      = 1.0 / 2.4;
// const float gamma_cutoffTo   = 0.04045;
// const float gamma_cutoffFrom = 0.0031308;
// const float gamma_div        = 12.92;

// Converts a color from linear light gamma to sRGB gamma
vec3 fromLinear(vec3 linearRGB)
{
    bvec3 cutoff = lessThan(linearRGB, vec3(gamma_cutoffFrom));
    vec3  higher = vec3(gamma_offset + 1.0) * pow(linearRGB, vec3(gamma_gamma)) - vec3(gamma_offset);
    vec3  lower  = linearRGB * vec3(gamma_div);

    return mix(higher, lower, cutoff);
}

// Wikipedia https://en.wikipedia.org/wiki/Rec._709
vec3 toLinear(vec3 sRGB)
{
    bvec3 cutoff = lessThan(sRGB, vec3(gamma_cutoffTo));
    vec3  higher = pow((sRGB + vec3(gamma_offset)) / vec3(gamma_offset + 1.0), vec3((1.0 / gamma_gamma)));
    vec3  lower  = sRGB / vec3(gamma_div);

    return mix(higher, lower, cutoff);
}