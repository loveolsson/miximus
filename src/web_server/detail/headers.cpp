#include "headers.hpp"
#include "utils/string_view.hpp"

#include <cassert>
#include <charconv>
#include <ranges>

namespace miximus::web_server::detail {

bool accept_encoding_has_gzip(std::string_view header) noexcept
{
    // Split the header into comma-separated parts
    for (const auto& p_str : std::views::split(header, ',')) {
        const std::string_view part(p_str.begin(), p_str.end());

        // Split the part into encoding and q value
        auto  enc      = part;
        float q        = 1.0F;
        auto  semi_pos = part.find_first_of(';');

        if (semi_pos != std::string_view::npos) {
            enc         = {part.data(), semi_pos};
            auto eq_pos = part.find_first_of('=', semi_pos);

            if (eq_pos != std::string_view::npos) {
                // Extract the range containing the q value
                auto one_past_eq = part.begin() + eq_pos + 1;
                auto q_str       = utils::ltrim_view({one_past_eq, part.end()});

                // Convert to float
                auto q_res = std::from_chars(q_str.data(), q_str.data() + q_str.size(), q);

                if (q_res.ec != std::errc() || q > 1.0F) {
                    // Malformed q-value, assume no gzip-support
                    return false;
                }
            }
        }

        enc = utils::trim_view(enc);

        if ((enc == "*" || utils::ascii_ieq_view(enc, "gzip")) && q > 0) {
            return true;
        }
    }

    return false;
}

} // namespace miximus::web_server::detail

#ifndef NDEBUG
namespace {

int tests() noexcept
{
    using namespace miximus::web_server::detail;

    assert(accept_encoding_has_gzip("") == false);
    assert(accept_encoding_has_gzip("gzip;q=1.0") == true);
    assert(accept_encoding_has_gzip("gzip;q=0.0") == false);
    assert(accept_encoding_has_gzip("*;q=1.0") == true);
    assert(accept_encoding_has_gzip("*;q=0.5") == true);
    assert(accept_encoding_has_gzip("*;q=1") == true);
    assert(accept_encoding_has_gzip("*;q=0.0") == false);
    assert(accept_encoding_has_gzip("*;q=0") == false);
    assert(accept_encoding_has_gzip("gzip") == true);
    assert(accept_encoding_has_gzip("deflate") == false);
    assert(accept_encoding_has_gzip("deflate, gzip;q=0.7, *;q=0.3") == true);
    assert(accept_encoding_has_gzip("deflate;q=1.0, br;q=0.8") == false);
    assert(accept_encoding_has_gzip("gzip;q=1.2") == false);
    assert(accept_encoding_has_gzip("gzip;q=abc") == false);
    assert(accept_encoding_has_gzip("gzip;q=") == false);

    return 0;
}

const auto run_tests = tests();

} // namespace
#endif
