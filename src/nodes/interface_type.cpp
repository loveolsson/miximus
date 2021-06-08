#include "nodes/interface_type.hpp"

#include <map>
#include <set>

namespace miximus::nodes {

static const std::map<interface_type_e, std::set<interface_type_e>> conversion_pairs{
    {
        interface_type_e::f64,   // to
        {interface_type_e::i64}, // from
    },
    {
        interface_type_e::i64,
        {interface_type_e::f64},
    },
};

bool test_interface_pair(interface_type_e from, interface_type_e to)
{
    if (from == to) {
        return true;
    }

    auto it = conversion_pairs.find(to);
    if (it == conversion_pairs.end()) {
        return false;
    }

    return it->second.count(from) > 0;
}

} // namespace miximus::nodes