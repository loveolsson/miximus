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
vec3 from_linear(vec3 linear_rgb)
{
    bvec3 cutoff = lessThan(linear_rgb, vec3(gamma_cutoffFrom));
    vec3  higher = vec3(gamma_offset + 1.0) * pow(linear_rgb, vec3(gamma_gamma)) - vec3(gamma_offset);
    vec3  lower  = linear_rgb * vec3(gamma_div);

    return mix(higher, lower, cutoff);
}

// Wikipedia https://en.wikipedia.org/wiki/Rec._709
vec3 to_linear(vec3 srgb)
{
    bvec3 cutoff = lessThan(srgb, vec3(gamma_cutoffTo));
    vec3  higher = pow((srgb + vec3(gamma_offset)) / vec3(gamma_offset + 1.0), vec3((1.0 / gamma_gamma)));
    vec3  lower  = srgb / vec3(gamma_div);

    return mix(higher, lower, cutoff);
}

// These values match channel_order_e and color_operation_e in the GPU API.
const uint channel_order_rgba = 0;
const uint channel_order_bgra = 1;
const uint channel_order_bgrx = 2;
const uint channel_order_argb = 3;

const int color_operation_none                         = 0;
const int color_operation_decode_rec709                = 1;
const int color_operation_encode_rec709                = 2;
const int color_operation_encode_rec709_premultiplied  = 3;
const int color_operation_decode_rec709_straight_alpha = 4;
const int color_operation_encode_rec709_straight_alpha = 5;
const int color_operation_decode_rec709_premultiplied  = 6;
const int color_operation_decode_rec709_ignore_alpha   = 7;
const int color_operation_encode_rec709_ignore_alpha   = 8;
const int color_operation_decode_srgb_premultiplied     = 9;
const int color_operation_encode_srgb_premultiplied     = 10;

vec4 map_input_channels(vec4 color, uint channel_order)
{
    switch (channel_order) {
        case channel_order_bgra:
            return color.bgra;
        case channel_order_bgrx:
            return vec4(color.bgr, 1.0);
        case channel_order_argb:
            return color.gbar;
        default:
            return color;
    }
}

vec4 map_output_channels(vec4 color, uint channel_order)
{
    switch (channel_order) {
        case channel_order_bgra:
            return color.bgra;
        case channel_order_bgrx:
            return vec4(color.bgr, 1.0);
        case channel_order_argb:
            return color.argb;
        default:
            return color;
    }
}

vec4 to_video_premultiplied(vec4 color)
{
    if (!(color.a > 0.0)) {
        return vec4(0.0);
    }

    return vec4(from_linear(color.rgb / color.a) * color.a, color.a);
}

vec4 to_linear_premultiplied(vec4 color)
{
    if (!(color.a > 0.0)) {
        return vec4(0.0);
    }

    return vec4(to_linear(color.rgb / color.a) * color.a, color.a);
}

vec4 srgb_to_linear_premultiplied(vec4 color)
{
    if (!(color.a > 0.0)) {
        return vec4(0.0);
    }

    // Decode straight sRGB, then premultiply in the working linear space.
    // Alpha is coverage and must not undergo the RGB transfer function.
    vec3 straight_rgb = clamp(color.rgb / color.a, 0.0, 1.0);
    vec3 linear_rgb = mix(pow((straight_rgb + 0.055) / 1.055, vec3(2.4)),
                          straight_rgb / 12.92,
                          lessThanEqual(straight_rgb, vec3(0.04045)));
    return vec4(linear_rgb * color.a, color.a);
}

vec4 to_video_straight(vec4 color)
{
    if (!(color.a > 0.0)) {
        return vec4(0.0);
    }

    return vec4(from_linear(color.rgb / color.a), color.a);
}

vec4 encode_rec709_ignore_alpha(vec4 color)
{
    // Recover straight RGB before discarding alpha. Zero alpha has no recoverable color.
    return vec4(to_video_straight(color).rgb, 1.0);
}

vec4 linear_to_srgb_premultiplied(vec4 color)
{
    if (!(color.a > 0.0)) {
        return vec4(0.0);
    }
    vec3 straight_rgb = clamp(color.rgb / color.a, 0.0, 1.0);
    vec3 encoded_rgb = mix(1.055 * pow(straight_rgb, vec3(1.0 / 2.4)) - 0.055,
                           12.92 * straight_rgb,
                           lessThanEqual(straight_rgb, vec3(0.0031308)));
    return vec4(encoded_rgb * color.a, color.a);
}
