#pragma once
#include <cassert>
#include <thread>

namespace miximus::utils {

class thread_guard_s
{
    static inline bool            initiated{};
    static inline std::thread::id id_;

  public:
    static void test()
    {
        auto id = std::this_thread::get_id();

        if (!initiated) {
            id_       = id;
            initiated = true;
        } else {
            assert(id == id_);
        }
    }
};

} // namespace miximus::utils
