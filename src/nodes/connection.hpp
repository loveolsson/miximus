#pragma once
#include "nodes/connection_type.hpp"

#include <memory>
#include <string>
#include <vector>

namespace miximus {

class node_manager;

struct node_connection
{
    const std::string id;
    const std::string node_id;
    const std::string iface;
};

class node_interface_base
{
  protected:
    std::vector<node_connection> connections_;

  public:
    virtual node_connection_type get_type() = 0;

    std::vector<std::shared_ptr<node_interface_base>> resolve_connections(node_manager& manager);
};

template <typename T>
class node_interface : public node_interface_base
{
    const T value_;

  public:
    node_connection_type get_type() final { return get_connection_type<T>(); }

    const T& value() const { return value_; }
};

} // namespace miximus