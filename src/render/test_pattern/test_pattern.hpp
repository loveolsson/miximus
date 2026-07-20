#pragma once
#include "render/surface/surface_fwd.hpp"

namespace miximus::render {

enum class test_pattern_e
{
    smpte_color_bars,
    ebu_color_bars,
    black_field,
    white_field,
    red_field,
    green_field,
    blue_field,
    grayscale_ramp,
    crosshatch,
    checkerboard,
    multiburst,
    zone_plate,
};

void render_test_pattern(surface_s& surface, test_pattern_e pattern);
void render_test_pattern_logo(surface_s& surface);

} // namespace miximus::render
