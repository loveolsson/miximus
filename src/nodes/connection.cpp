#include "connection.hpp"
#include <boost/functional/hash.hpp>

namespace miximus::nodes {
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