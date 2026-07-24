#include "headers.hpp"

#include "utils/string_view.hpp"

#include <cassert>
#include <chrono>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace {

constexpr int QUALITY_MAX = 1000;

std::optional<int> parse_quality(std::string_view value) noexcept
{
    value = miximus::utils::trim_view(value);
    if (value.empty() || (value.front() != '0' && value.front() != '1')) {
        return std::nullopt;
    }

    const bool is_one = value.front() == '1';
    value.remove_prefix(1);
    if (value.empty()) {
        return is_one ? QUALITY_MAX : 0;
    }
    if (value.front() != '.') {
        return std::nullopt;
    }

    value.remove_prefix(1);
    if (value.size() > 3) {
        return std::nullopt;
    }

    int quality = 0;
    int place   = 100;
    for (const char digit : value) {
        if (digit < '0' || digit > '9' || (is_one && digit != '0')) {
            return std::nullopt;
        }
        quality += (digit - '0') * place;
        place /= 10;
    }

    return is_one ? QUALITY_MAX : quality;
}

struct encoding_quality_s
{
    std::optional<int> gzip;
    std::optional<int> identity;
    std::optional<int> wildcard;
};

encoding_quality_s parse_accept_encoding(std::string_view header) noexcept
{
    encoding_quality_s quality;

    for (const auto& part_range : std::views::split(header, ',')) {
        auto part = miximus::utils::trim_view(std::string_view(part_range.begin(), part_range.end()));
        if (part.empty()) {
            continue;
        }

        auto token       = part;
        int  token_value = QUALITY_MAX;
        if (const auto semicolon = part.find(';'); semicolon != std::string_view::npos) {
            token = miximus::utils::trim_view(part.substr(0, semicolon));

            auto parameter = miximus::utils::trim_view(part.substr(semicolon + 1));
            if (parameter.size() < 2 || (parameter.front() != 'q' && parameter.front() != 'Q')) {
                continue;
            }
            parameter.remove_prefix(1);
            parameter = miximus::utils::ltrim_view(parameter);
            if (parameter.empty() || parameter.front() != '=') {
                continue;
            }
            parameter.remove_prefix(1);

            const auto parsed = parse_quality(parameter);
            if (!parsed.has_value()) {
                continue;
            }
            token_value = *parsed;
        }

        std::optional<int>* destination = nullptr;
        if (miximus::utils::ascii_ieq_view(token, "gzip") || miximus::utils::ascii_ieq_view(token, "x-gzip")) {
            destination = &quality.gzip;
        } else if (miximus::utils::ascii_ieq_view(token, "identity")) {
            destination = &quality.identity;
        } else if (token == "*") {
            destination = &quality.wildcard;
        }

        if (destination != nullptr && !destination->has_value()) {
            *destination = token_value;
        }
    }

    return quality;
}

std::string_view strip_weak_prefix(std::string_view etag) noexcept
{
    etag = miximus::utils::trim_view(etag);
    if (etag.starts_with("W/")) {
        etag.remove_prefix(2);
    }
    return etag;
}

} // namespace

namespace miximus::web_server::detail {

content_encoding_e select_content_encoding(std::string_view header) noexcept
{
    if (header.empty()) {
        return content_encoding_e::identity;
    }

    const auto quality          = parse_accept_encoding(header);
    const int  gzip_quality     = quality.gzip.value_or(quality.wildcard.value_or(0));
    const int  identity_quality = quality.identity.value_or(quality.wildcard == 0 ? 0 : QUALITY_MAX);

    if (gzip_quality > 0 && gzip_quality >= identity_quality) {
        return content_encoding_e::gzip;
    }
    if (identity_quality > 0) {
        return content_encoding_e::identity;
    }
    return content_encoding_e::not_acceptable;
}

bool if_none_match_matches(std::string_view header, std::string_view etag) noexcept
{
    etag = strip_weak_prefix(etag);

    while (!header.empty()) {
        header = utils::ltrim_view(header);
        if (header.empty()) {
            break;
        }
        bool   inside_tag = false;
        size_t comma      = std::string_view::npos;
        for (size_t index = 0; index < header.size(); ++index) {
            if (header[index] == '"') {
                inside_tag = !inside_tag;
            } else if (header[index] == ',' && !inside_tag) {
                comma = index;
                break;
            }
        }

        const auto candidate = strip_weak_prefix(header.substr(0, comma));
        if (candidate == "*" || candidate == etag) {
            return true;
        }

        if (comma == std::string_view::npos) {
            break;
        }
        header.remove_prefix(comma + 1);
    }

    return false;
}

std::string make_http_date()
{
    return std::format("{:%a, %d %b %Y %T GMT}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

} // namespace miximus::web_server::detail

#ifndef NDEBUG
namespace {

int tests() noexcept
{
    using namespace miximus::web_server::detail;

    assert(select_content_encoding("") == content_encoding_e::identity);
    assert(select_content_encoding("gzip") == content_encoding_e::gzip);
    assert(select_content_encoding("gzip;q=0") == content_encoding_e::identity);
    assert(select_content_encoding("gzip;q=0.5") == content_encoding_e::identity);
    assert(select_content_encoding("gzip;q=1, identity;q=0.5") == content_encoding_e::gzip);
    assert(select_content_encoding("*;q=1") == content_encoding_e::gzip);
    assert(select_content_encoding("*;q=0") == content_encoding_e::not_acceptable);
    assert(select_content_encoding("gzip;q=0, identity;q=0") == content_encoding_e::not_acceptable);
    assert(select_content_encoding("gzip;q=1.2") == content_encoding_e::identity);
    assert(select_content_encoding("gzip;q=abc") == content_encoding_e::identity);
    assert(select_content_encoding("gzip;q=1.0000") == content_encoding_e::identity);
    assert(select_content_encoding("gzip;q=0, *;q=1") == content_encoding_e::identity);

    assert(if_none_match_matches("\"one\"", "\"one\""));
    assert(if_none_match_matches("W/\"one\"", "\"one\""));
    assert(if_none_match_matches("\"other\", W/\"one\"", "\"one\""));
    assert(if_none_match_matches("*", "\"one\""));
    assert(!if_none_match_matches("*invalid", "\"one\""));
    assert(!if_none_match_matches("\"other\"", "\"one\""));

    return 0;
}

const auto run_tests = tests();

} // namespace
#endif
