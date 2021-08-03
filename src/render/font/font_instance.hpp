#pragma once
#include "font_info.hpp"

namespace miximus::render::font {

class font_loader_s;

class font_instance_s
{
    bool  valid_{};
    void* face{};

    friend class font_loader_s;

  public:
    font_instance_s(void* l, const std::filesystem::path&, int index);
    ~font_instance_s();

    void set_size(int size_in_px);
};

} // namespace miximus::render::font
