#include "clock_source.hpp"

#include <chrono>
#include <thread>

namespace miximus::core {

utils::flicks steady_clock_source_s::now() const { return utils::flicks_now(); }

void steady_clock_source_s::wait_until(utils::flicks time)
{
    const auto target =
        std::chrono::steady_clock::time_point{std::chrono::duration_cast<std::chrono::steady_clock::duration>(time)};
    std::this_thread::sleep_until(target);
}

std::string_view steady_clock_source_s::name() const { return "Internal"; }

} // namespace miximus::core
