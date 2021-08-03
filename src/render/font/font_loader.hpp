#pragma once
#include "font_instance.hpp"

#include <memory>

namespace miximus::render::font {

class font_loader_s
{
    void* library_;

  public:
    font_loader_s();
    ~font_loader_s();

    std::unique_ptr<font_instance_s> load_font(const font_variant_s* face);
};

} // namespace miximus::render::font
