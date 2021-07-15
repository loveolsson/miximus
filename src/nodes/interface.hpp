#pragma once
#include "gpu/types.hpp"
#include "interface_type.hpp"
#include "types/connection_set.hpp"
#include "types/node_map.hpp"

#include <climits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace miximus::nodes {

class interface_i
{
  public:
    enum class dir
    {
        input,
        output,
    };

  private:
    const dir direction_;
    int       max_connection_count_;

  public:
    interface_i(dir direction)
        : direction_(direction)
    {
    }

    virtual ~interface_i() {}

    bool         add_connection(con_set_t* connections, const connection& con, con_set_t& removed) const;
    interface_i* resolve_connection(const node_map_t&, const con_set_t&);
    dir          direction() { return direction_; }

    virtual bool has_value() const = 0;
    virtual void reset()           = 0;

    virtual interface_type_e type()                         = 0;
    virtual bool             accepts(interface_type_e type) = 0;
};

template <typename T>
class interface : public interface_i
{
    std::optional<T> value_;

  public:
    interface(dir direction)
        : interface_i(direction){};
    ~interface(){};

    bool             resolve_connection_value(const node_map_t&, const con_set_t&);
    bool             has_value() const final { return value_ != std::nullopt; }
    void             reset() final { value_ = std::nullopt; }
    interface_type_e type() final { return get_interface_type<T>(); }
    virtual bool     accepts(interface_type_e type) final;

    const T get_value() const
    {
        if (!has_value()) {
            // Since this happens after resolve, the value should always be set
            assert(false);
            return T();
        }

        return *value_;
    }

    void set_value(const T& value) { value_ = value; }
};

} // namespace miximus::nodes