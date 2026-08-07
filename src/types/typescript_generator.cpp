#include "action.hpp"
#include "connection.hpp"
#include "error.hpp"
#include "gpu/types.hpp"
#include "json_contract.hpp"
#include "json_contract_descriptions.hpp"
#include "node_status.hpp"
#include "topic.hpp"
#include "web_message.hpp"
#include "web_message_request.hpp"

#include <boost/describe.hpp>
#include <boost/mp11.hpp>

#include <concepts>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace miximus::gpu {

BOOST_DESCRIBE_STRUCT(rect_s, (), (pos, size))

} // namespace miximus::gpu

namespace {
using namespace miximus;

template <typename>
inline constexpr bool always_false = false;

template <typename T>
struct vector_traits
{
    static constexpr bool value = false;
};

template <typename T, typename Allocator>
struct vector_traits<std::vector<T, Allocator>>
{
    static constexpr bool value = true;
    using value_type            = T;
};

template <typename T>
struct unordered_map_traits
{
    static constexpr bool value = false;
};

template <typename Key, typename Value, typename Hash, typename Equal, typename Allocator>
struct unordered_map_traits<std::unordered_map<Key, Value, Hash, Equal, Allocator>>
{
    static constexpr bool value = true;
    using key_type              = Key;
    using value_type            = Value;
};

template <typename T>
std::string typescript_type()
{
    if constexpr (std::same_as<T, bool>) {
        return "boolean";
    } else if constexpr (std::integral<T> || std::floating_point<T> || std::same_as<T, utils::flicks>) {
        return "number";
    } else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view>) {
        return "string";
    } else if constexpr (std::same_as<T, action_e>) {
        return "action_e";
    } else if constexpr (std::same_as<T, topic_e>) {
        return "topic_e";
    } else if constexpr (std::same_as<T, error_e>) {
        return "error_e";
    } else if constexpr (std::same_as<T, font_registry_command_e>) {
        return "font_registry_command_e";
    } else if constexpr (miximus::detail::optional_traits<T>::value) {
        return typescript_type<typename miximus::detail::optional_traits<T>::value_type>() + " | null";
    } else if constexpr (vector_traits<T>::value) {
        return "readonly " + typescript_type<typename vector_traits<T>::value_type>() + "[]";
    } else if constexpr (std::same_as<T, frame_rate_s>) {
        return "frame_rate_s";
    } else if constexpr (std::same_as<T, settings_option_s>) {
        return "settings_option_s";
    } else if constexpr (std::same_as<T, gpu::vec2_t>) {
        return "vec2_t";
    } else if constexpr (std::same_as<T, connection_s>) {
        return "connection_s";
    } else if constexpr (std::same_as<T, web_message::node_s>) {
        return "node_s";
    } else if constexpr (std::same_as<T, web_message::config_s>) {
        return "config_s";
    } else if constexpr (unordered_map_traits<T>::value) {
        static_assert(std::same_as<typename unordered_map_traits<T>::key_type, std::string>);
        static_assert(std::same_as<typename unordered_map_traits<T>::value_type, nlohmann::json>);
        return "Readonly<Record<string, node_status_s>>";
    } else {
        static_assert(always_false<T>, "Unsupported TypeScript contract member type");
    }
}

template <typename Object, typename Member>
std::string typescript_member_type()
{
    if constexpr (std::same_as<Member, nlohmann::json>) {
        if constexpr (std::same_as<Object, web_message::node_s> ||
                      std::same_as<Object, web_message::add_node_request_s> ||
                      std::same_as<Object, web_message::update_node_command_s> ||
                      std::same_as<Object, web_message::update_node_request_s>) {
            return "options_s";
        } else if constexpr (std::same_as<Object, web_message::node_status_result_s> ||
                             std::same_as<Object, web_message::node_status_command_s>) {
            return "node_status_s";
        } else {
            static_assert(always_false<Object>, "Opaque JSON contract member needs a TypeScript type");
        }
    } else {
        return typescript_type<Member>();
    }
}

template <typename T>
void emit_interface(std::ostream& output, std::string_view name)
{
    static_assert(boost::describe::has_describe_members<T>::value);

    output << "export interface " << name << " {\n";
    if constexpr (requires { typename std::integral_constant<action_e, T::action>; }) {
        output << "  readonly action: action_e." << enum_to_string(T::action) << ";\n";
    }
    if constexpr (requires { typename std::integral_constant<topic_e, T::topic>; }) {
        output << "  readonly topic: topic_e." << enum_to_string(T::topic) << ";\n";
    }

    using members_t =
        boost::describe::describe_members<T, boost::describe::mod_public | boost::describe::mod_inherited>;
    boost::mp11::mp_for_each<members_t>([&](auto member) {
        using member_t = std::remove_cvref_t<decltype(std::declval<T>().*member.pointer)>;
        output << "  readonly " << member.name << (miximus::detail::optional_traits<member_t>::value ? "?" : "") << ": "
               << typescript_member_type<T, member_t>() << ";\n";
    });
    output << "}\n\n";
}

template <typename T>
void emit_enum(std::ostream& output, std::string_view name)
{
    static_assert(boost::describe::has_describe_enumerators<T>::value);

    output << "export const enum " << name << " {\n";
    using enumerators_t = boost::describe::describe_enumerators<T>;
    boost::mp11::mp_for_each<enumerators_t>(
        [&](auto enumerator) { output << "  " << enumerator.name << " = \"" << enumerator.name << "\",\n"; });
    output << "}\n\n";
}

template <typename T>
struct status_contract_s
{
    std::string_view name;
};

template <typename... T>
void emit_status_contracts(std::ostream& output, status_contract_s<T>... contracts)
{
    static_assert(sizeof...(T) > 0);

    (emit_interface<T>(output, contracts.name), ...);

    output << "// A status payload is a sparse delta. The intersection forms one catalog of\n"
              "// known keys; Partial makes every key optional for individual node types and updates.\n"
              "// prettier-ignore\n"
              "export type node_status_s = Partial<\n";
    size_t remaining = sizeof...(T);
    ((output << "  " << contracts.name << (--remaining == 0 ? "\n>;\n" : " &\n")), ...);
}

std::string generate_typescript()
{
    std::ostringstream output;
    output << "// Generated from the described C++ JSON contracts. Do not edit manually.\n\n";

#define EMIT_TYPE(type) emit_interface<type>(output, #type)
#define EMIT_NAMESPACED_TYPE(namespace_name, type) emit_interface<namespace_name::type>(output, #type)

    emit_enum<action_e>(output, "action_e");
    emit_enum<topic_e>(output, "topic_e");
    emit_enum<error_e>(output, "error_e");
    emit_enum<font_registry_command_e>(output, "font_registry_command_e");
    EMIT_TYPE(settings_option_s);
    EMIT_TYPE(frame_rate_s);
    output << "export type vec2_t = [number, number];\n\n"
              "export interface options_s {\n"
              "  readonly node_visual_position?: vec2_t;\n"
              "  readonly name?: string;\n"
              "  readonly [key: string]: unknown;\n"
              "}\n\n";
    EMIT_NAMESPACED_TYPE(web_message, message_s);
    EMIT_NAMESPACED_TYPE(web_message, command_s);
    EMIT_TYPE(connection_s);
    EMIT_NAMESPACED_TYPE(web_message, node_s);
    EMIT_NAMESPACED_TYPE(web_message, config_s);
    EMIT_NAMESPACED_TYPE(web_message, subscribe_request_s);
    EMIT_NAMESPACED_TYPE(web_message, unsubscribe_request_s);
    EMIT_NAMESPACED_TYPE(web_message, add_node_request_s);
    EMIT_NAMESPACED_TYPE(web_message, remove_node_request_s);
    EMIT_NAMESPACED_TYPE(web_message, update_node_request_s);
    EMIT_NAMESPACED_TYPE(web_message, add_connection_request_s);
    EMIT_NAMESPACED_TYPE(web_message, remove_connection_request_s);
    EMIT_NAMESPACED_TYPE(web_message, font_registry_request_s);
    EMIT_NAMESPACED_TYPE(web_message, config_request_s);
    EMIT_NAMESPACED_TYPE(web_message, node_status_request_s);
    EMIT_NAMESPACED_TYPE(web_message, ping_response_s);
    EMIT_NAMESPACED_TYPE(web_message, socket_info_s);
    EMIT_NAMESPACED_TYPE(web_message, result_s);
    EMIT_NAMESPACED_TYPE(web_message, config_result_s);
    EMIT_NAMESPACED_TYPE(web_message, node_status_result_s);
    EMIT_NAMESPACED_TYPE(web_message, error_s);
    EMIT_NAMESPACED_TYPE(web_message, add_node_command_s);
    EMIT_NAMESPACED_TYPE(web_message, remove_node_command_s);
    EMIT_NAMESPACED_TYPE(web_message, update_node_command_s);
    EMIT_NAMESPACED_TYPE(web_message, add_connection_command_s);
    EMIT_NAMESPACED_TYPE(web_message, remove_connection_command_s);
    EMIT_NAMESPACED_TYPE(web_message, node_status_command_s);
    EMIT_NAMESPACED_TYPE(gpu, rect_s);

#define STATUS_CONTRACT(type) status_contract_s<status::type>{#type}
    emit_status_contracts(output,
                          STATUS_CONTRACT(connected_status_s),
                          STATUS_CONTRACT(device_names_status_s),
                          STATUS_CONTRACT(display_modes_status_s),
                          STATUS_CONTRACT(source_names_status_s),
                          STATUS_CONTRACT(monitor_options_status_s),
                          STATUS_CONTRACT(font_names_status_s),
                          STATUS_CONTRACT(font_variants_status_s),
                          STATUS_CONTRACT(application_frame_status_s),
                          STATUS_CONTRACT(application_lifecycle_status_s),
                          STATUS_CONTRACT(application_scheduler_status_s),
                          STATUS_CONTRACT(render_delay_test_status_s),
                          STATUS_CONTRACT(source_timing_status_s),
                          STATUS_CONTRACT(decklink_input_device_status_s),
                          STATUS_CONTRACT(decklink_output_device_status_s),
                          STATUS_CONTRACT(decklink_input_metrics_status_s),
                          STATUS_CONTRACT(ndi_input_metrics_status_s),
                          STATUS_CONTRACT(download_stream_status_s),
                          STATUS_CONTRACT(ndi_output_metrics_status_s),
                          STATUS_CONTRACT(decklink_output_metrics_status_s),
                          STATUS_CONTRACT(screen_output_metrics_status_s));
#undef STATUS_CONTRACT

#undef EMIT_NAMESPACED_TYPE
#undef EMIT_TYPE

    return std::move(output).str();
}

bool write_if_changed(const std::filesystem::path& path, std::string_view contents)
{
    {
        std::ifstream input(path, std::ios::binary);
        if (input) {
            const std::string current(std::istreambuf_iterator<char>(input), {});
            if (current == contents) {
                return false;
            }
        }
    }

    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not open TypeScript contract output");
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: miximus_typescript_generator <output.ts>\n";
        return 1;
    }

    try {
        const auto changed = write_if_changed(argv[1], generate_typescript());
        std::cout << (changed ? "Generated " : "Unchanged ") << argv[1] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TypeScript contract generation failed: " << error.what() << '\n';
        return 1;
    }
}
