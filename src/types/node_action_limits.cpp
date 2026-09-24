#include "node_action_limits.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace miximus::node_action_limits {
bool valid_payload(const nlohmann::json& payload)
{
    // Bound traversal before serialization: a small but deeply nested document
    // must not overflow the serializer's stack. Node results use the same limits.
    struct entry_s
    {
        const nlohmann::json* value;
        size_t                depth;
    };
    std::vector<entry_s> pending{
        {&payload, 0}
    };
    size_t values = 1;
    size_t string_bytes{};
    while (!pending.empty()) {
        const auto [value, depth] = pending.back();
        pending.pop_back();
        if (depth > MAX_JSON_DEPTH || value->is_binary() || value->is_discarded()) {
            return false;
        }
        if (value->is_string()) {
            string_bytes += value->get_ref<const std::string&>().size();
        } else if (value->is_structured()) {
            if (value->size() > MAX_JSON_VALUES - values) {
                return false;
            }
            values += value->size();
            for (auto it = value->begin(); it != value->end(); ++it) {
                if (value->is_object()) {
                    string_bytes += it.key().size();
                }
                pending.push_back({&it.value(), depth + 1});
            }
        }
        if (string_bytes > MAX_PAYLOAD_BYTES) {
            return false;
        }
    }
    try {
        return payload.dump().size() <= MAX_PAYLOAD_BYTES;
    } catch (const nlohmann::json::exception&) {
        return false; // Native callers may supply strings that are not valid UTF-8.
    }
}

} // namespace miximus::node_action_limits
