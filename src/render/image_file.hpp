#pragma once

#include "wrapper/stb/image.hpp"

#include <string>

namespace miximus::render {

// File I/O, stb decoding and sRGB-to-linear premultiplication. Run on a CPU worker.
stb::decoded_image_s load_image_file(const std::string& path_utf8);

} // namespace miximus::render
