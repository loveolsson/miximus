#include "connection.hpp"
#include <boost/functional/hash.hpp>
#include <nlohmann/json.hpp>

namespace miximus::nodes {

nlohmann::json connection::serialize()
{
    return {
        {"from_node", from_node},
        {"from_interface", from_interface},
        {"to_node", to_node},
        {"to_interface", to_interface},
    };
}

std::size_t connection_hash::operator()(const miximus::nodes::connection& c) const
{
    using boost::hash_combine;
    size_t res = 0;

    hash_combine(res, c.from_node);
    hash_combine(res, c.from_interface);
    hash_combine(res, c.to_node);
    hash_combine(res, c.to_interface);

    return res;
}
} // namespace miximus::nodes