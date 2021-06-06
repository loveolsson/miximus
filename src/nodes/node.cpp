#include "nodes/node.hpp"
#include "logger/logger.hpp"
#include "nodes/dummy/dummy.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes {

bool node::set_option(std::string_view option, const nlohmann::json& val)
{
    if (option == "position") {
        if (!val.is_array()) {
            return false;
        }

        if (val.size() != 2) {
            return false;
        }

        auto& x = val[0];
        auto& y = val[1];

        if (!x.is_number() || !y.is_number()) {
            return false;
        }

        position_ = {x.get<double>(), y.get<double>()};
        return true;
    }

    return false;
}

nlohmann::json node::get_options()
{
    using namespace nlohmann;

    return {
        {"position", json::array({position_.x, position_.y})},
    };
}

std::shared_ptr<node> create_node(node_type_t type, const std::string& id, message::error_t& error)
{
    switch (type) {
        case node_type_t::decklink_producer:
            error = message::error_t::invalid_type;
            return nullptr;
        case node_type_t::decklink_consumer:
            error = message::error_t::invalid_type;
            return nullptr;

        default:
#if 0
            error = message::error_t::invalid_type;
            return nullptr;
#else
            return dummy::create_node(id);
#endif
    }
}

} // namespace miximus::nodes