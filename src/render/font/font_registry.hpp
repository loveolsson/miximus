#pragma once
#include "font_info.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace miximus::render {

constexpr std::string_view get_default_font_name()
{
#ifdef __linux__
    return "Liberation Sans";
#else
    return "Arial";
#endif
}

class font_registry_s
{
    using font_map_t = std::map<std::string, font_info_s, std::less<>>;

    font_map_t                fonts_;
    std::atomic<uint64_t>     font_list_version_{0};
    mutable std::shared_mutex font_mutex_;

    static void       log_fonts(const font_map_t& fonts);
    static font_map_t scan_fonts();

  public:
    font_registry_s();
    ~font_registry_s() = default;

    void refresh();

    uint64_t get_font_list_version() const { return font_list_version_.load(std::memory_order_relaxed); }

    std::optional<font_info_s>    find_font(std::string_view name) const;
    std::optional<font_variant_s> find_font_variant(std::string_view name, std::string_view variant) const;
    std::vector<std::string>      get_font_names() const;
    std::vector<std::string>      get_font_variant_names(std::string_view name) const;

    static std::unique_ptr<font_registry_s> create_font_registry();
};

} // namespace miximus::render
