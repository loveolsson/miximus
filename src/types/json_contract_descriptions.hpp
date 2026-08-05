#pragma once
#include "frame_rate.hpp"
#include "settings_option.hpp"

#include <boost/describe.hpp>

namespace miximus {

BOOST_DESCRIBE_STRUCT(frame_rate_s, (), (numerator, denominator))
BOOST_DESCRIBE_STRUCT(settings_option_s, (), (id, label))

} // namespace miximus
