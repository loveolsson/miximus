// Converts a color from linear light gamma to sRGB gamma
vec3 fromLinear(vec3 linearRGB)
{
    bvec3 cutoff = lessThan(linearRGB, vec3(0.018));
    vec3  higher = vec3(1.099) * pow(linearRGB, vec3(0.45)) - vec3(0.099);
    vec3  lower  = linearRGB * vec3(4.5);

    return mix(higher, lower, cutoff);
}

// Converts a color from sRGB gamma to linear light gamma
// This document suggests E^2.4, with no cutoff. Needs further investigation:
// https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2087-0-201510-I!!PDF-E.pdf
// vec3 toLinear(vec3 sRGB)
// {
//     bvec3 cutoff = lessThan(sRGB, vec3(0.04045));
//     vec3  higher = pow((sRGB + vec3(0.055)) / vec3(1.055), vec3(2.4));
//     vec3  lower  = sRGB / vec3(12.92);

//     return mix(higher, lower, cutoff);
// }

// Wikipedia https://en.wikipedia.org/wiki/Rec._709
vec3 toLinear(vec3 sRGB)
{
    bvec3 cutoff = lessThan(sRGB, vec3(0.081));
    vec3  higher = pow((sRGB + vec3(0.099)) / vec3(1.099), vec3((1.0 / 0.45)));
    vec3  lower  = sRGB / vec3(4.5);

    return mix(higher, lower, cutoff);
}