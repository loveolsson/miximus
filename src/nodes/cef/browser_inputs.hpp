#pragma once
#include "media_input_types.hpp"
#include "nodes/interface.hpp"
namespace miximus::nodes::cef {
struct browser_inputs_s
{
    std::array<input_interface_s<const gpu::texture_s*>, 8> ports;
    explicit browser_inputs_s(node_i& owner)
        : ports{
              {{owner, MEDIA_INPUT_NAMES[0]},
               {owner, MEDIA_INPUT_NAMES[1]},
               {owner, MEDIA_INPUT_NAMES[2]},
               {owner, MEDIA_INPUT_NAMES[3]},
               {owner, MEDIA_INPUT_NAMES[4]},
               {owner, MEDIA_INPUT_NAMES[5]},
               {owner, MEDIA_INPUT_NAMES[6]},
               {owner, MEDIA_INPUT_NAMES[7]}}
    }
    {
    }
};
} // namespace miximus::nodes::cef
