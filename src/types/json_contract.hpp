#pragma once
#include "utils/lookup.hpp"

#include <boost/describe.hpp>
#include <boost/mp11.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace miximus {

template <typename T>
concept described_json_object = boost::describe::has_describe_members<T>::value;

template <typename T>
concept described_json_enum = boost::describe::has_describe_enumerators<T>::value;

namespace detail {

template <typename T>
struct optional_traits
{
    static constexpr bool value = false;
};

template <typename T>
struct optional_traits<std::optional<T>>
{
    static constexpr bool value = true;
    using value_type            = T;
};

template <described_json_object T>
void write_described_json(nlohmann::json& json, const T& value, bool omit_empty_optionals = false)
{
    json = nlohmann::json::object();
    using members_t =
        boost::describe::describe_members<T, boost::describe::mod_public | boost::describe::mod_inherited>;
    boost::mp11::mp_for_each<members_t>([&](auto member) {
        const auto& member_value = value.*member.pointer;
        using member_t           = std::remove_cvref_t<decltype(member_value)>;
        if constexpr (optional_traits<member_t>::value) {
            if (member_value.has_value()) {
                json[member.name] = *member_value;
            } else if (!omit_empty_optionals) {
                json[member.name] = nullptr;
            }
        } else {
            json[member.name] = member_value;
        }
    });
}

template <described_json_object T>
void read_described_json(const nlohmann::json& json, T& value)
{
    using members_t =
        boost::describe::describe_members<T, boost::describe::mod_public | boost::describe::mod_inherited>;
    boost::mp11::mp_for_each<members_t>([&](auto member) {
        auto& member_value = value.*member.pointer;
        using member_t     = std::remove_cvref_t<decltype(member_value)>;
        if constexpr (optional_traits<member_t>::value) {
            const auto item = json.find(member.name);
            if (item == json.cend() || item->is_null()) {
                member_value.reset();
            } else {
                member_value = item->template get<typename optional_traits<member_t>::value_type>();
            }
        } else {
            json.at(member.name).get_to(member_value);
        }
    });
}

} // namespace detail

template <described_json_enum T>
void to_json(nlohmann::json& json, const T& value)
{
    const auto name = enum_to_string(value);
    if (name.empty()) {
        throw std::invalid_argument("Invalid enumerator in a JSON contract");
    }
    json = name;
}

template <described_json_enum T>
void from_json(const nlohmann::json& json, T& value)
{
    const auto name = json.get<std::string_view>();
    const auto result = enum_from_string<T>(name);
    if (!result.has_value()) {
        throw std::invalid_argument("Invalid enumerator in a JSON contract");
    }
    value = *result;
}

template <described_json_object T>
void to_json(nlohmann::json& json, const T& value)
{
    detail::write_described_json(json, value);
}

template <described_json_object T>
void from_json(const nlohmann::json& json, T& value)
{
    detail::read_described_json(json, value);
}

} // namespace miximus
