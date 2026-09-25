#include "typescript_generator.hpp"

#include "action.hpp"
#include "connection.hpp"
#include "error.hpp"
#include "gpu/types.hpp"
#include "json_contract.hpp"
#include "json_contract_descriptions.hpp"
#include "node_status.hpp"
#include "topic.hpp"
#include "utils/process_id.hpp"
#include "web_message.hpp"
#include "web_message_request.hpp"

#include <boost/describe.hpp>
#include <boost/mp11.hpp>
#include <magic_enum/magic_enum.hpp>

#include <concepts>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace miximus::gpu {

BOOST_DESCRIBE_STRUCT(rect_s, (), (pos, size))

} // namespace miximus::gpu

namespace miximus::typescript {
namespace {

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
               << typescript_member_type<member_t, member.pointer>() << ";\n";
    });
    output << "}\n\n";
}

template <typename T>
void emit_enum(std::ostream& output, std::string_view name)
{
    static_assert(std::is_enum_v<T>);

    output << "export const enum " << name << " {\n";
    for (const auto enum_name : magic_enum::enum_names<T>()) {
        output << "  " << enum_name << " = \"" << enum_name << "\",\n";
    }
    output << "}\n\n";
}

template <typename... T>
void emit_status_contracts(std::ostream& output, status::contract_s<T>... contracts)
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

} // namespace

std::string generate_typescript()
{
    std::ostringstream output;
    output << "// Generated from the described C++ JSON contracts. Do not edit manually.\n\n";

#define EMIT_TYPE(type) emit_interface<type>(output, #type)
#define EMIT_NAMESPACED_TYPE(namespace_name, type) emit_interface<namespace_name::type>(output, #type)

    emit_enum<cef_state_e>(output, "cef_state_e");
    emit_enum<cef_input_state_e>(output, "cef_input_state_e");
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
    EMIT_NAMESPACED_TYPE(web_message, node_action_request_s);
    EMIT_NAMESPACED_TYPE(web_message, node_action_result_s);
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

    std::apply([&](auto... contract) { emit_status_contracts(output, contract...); }, status::contracts);

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

    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }

    // Stage beside the destination so rename replaces it only after a complete write.
    auto staging = path;
    staging += ".tmp." + std::to_string(utils::process_id());
    if (!std::filesystem::create_directory(staging)) {
        throw std::runtime_error("Could not create TypeScript contract staging directory");
    }
    try {
        const auto    temporary = staging / "contracts.ts";
        std::ofstream output;
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.open(temporary, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        std::filesystem::rename(temporary, path);
        std::filesystem::remove(staging);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        throw;
    }
    return true;
}

} // namespace miximus::typescript
