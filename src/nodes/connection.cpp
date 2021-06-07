#include "connection.hpp"
#include <boost/functional/hash.hpp>

namespace miximus::nodes {

} // namespace miximus::nodes

namespace std {
std::size_t hash<miximus::nodes::connection>::operator()(const miximus::nodes::connection& c) const
{
    using namespace boost;
    size_t res = 0;

    hash_combine(res, c.from_node);
    hash_combine(res, c.from_interface);
    hash_combine(res, c.to_node);
    hash_combine(res, c.to_interface);

    return res;
}
} // namespace std