#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace miximus::nodes {

// Each frame resolves fresh graph values. Upstream output names remain stable
// throughout downstream execution, so borrowed names point directly to their
// owner. Copies retain that pointer; new names are constructed as owned values.
// Text access is exposed through string views.
class source_name_s
{
    std::variant<std::string, const source_name_s*> value_;

    const source_name_s* owner() const noexcept
    {
        if (const auto* reference = std::get_if<const source_name_s*>(&value_)) {
            return *reference;
        }
        return this;
    }

  public:
    source_name_s() = default;
    source_name_s(std::string_view value)
        : value_(std::in_place_type<std::string>, value)
    {
    }
    // Explicit borrowing avoids references to ordinary local copies. The
    // returned value points to the ultimate owner, not an intermediate wrapper.
    [[nodiscard]] source_name_s borrow() const& noexcept
    {
        source_name_s result;
        result.value_ = owner();
        return result;
    }
    source_name_s borrow() const&& = delete;

    [[nodiscard]] bool is_borrowed() const noexcept { return std::holds_alternative<const source_name_s*>(value_); }
    [[nodiscard]] source_name_s owned_copy() const { return source_name_s(view()); }

    [[nodiscard]] size_t           size() const noexcept { return view().size(); }
    [[nodiscard]] bool             empty() const noexcept { return view().empty(); }
    [[nodiscard]] std::string_view view() const noexcept { return std::get<std::string>(owner()->value_); }
    bool operator==(const source_name_s& other) const noexcept { return view() == other.view(); }
};

} // namespace miximus::nodes
