#pragma once
#include "nodes/connection.hpp"
#include "nodes/node_type.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace miximus {

class node
{
  protected:
    const std::string id_;

    node(const std::string& id)
        : id_(id)
    {
    }

    virtual ~node() {}

  public:
    virtual bool                                 set_options(const nlohmann::json&)   = 0;
    virtual bool                                 test_connection(std::string)         = 0;
    virtual std::shared_ptr<node_interface_base> get_interface(std::string_view name) = 0;

    const std::string& id() const;
};

class node_factory
{
  public:
    static std::shared_ptr<node> create_node(node_type_t type, std::string id);
};
} // namespace miximus