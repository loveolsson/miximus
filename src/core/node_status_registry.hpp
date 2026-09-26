#pragma once
#include "node_status_handle.hpp"
#include "types/node_status_json.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace miximus::core {

class node_status_registry_s
{
  public:
    struct status_update_s
    {
        std::string    node_id;
        nlohmann::json status;
    };
    using callback_t = std::function<void(const std::vector<status_update_s>&)>;

  private:
    struct value_i
    {
        virtual ~value_i()                       = default;
        virtual nlohmann::json serialize() const = 0;
    };
    template <status::registered_contract T>
    struct value_s final : value_i
    {
        T value;
        explicit value_s(T input)
            : value(std::move(input))
        {
        }
        nlohmann::json serialize() const final { return value; }
    };
    struct state_s;
    std::shared_ptr<state_s> state_;
    void        enqueue(const node_status_handle_s& node, std::type_index type, std::unique_ptr<value_i> value);
    static void schedule(const std::shared_ptr<state_s>& state);
    static void drain(const std::shared_ptr<state_s>& state);

  public:
    explicit node_status_registry_s(boost::asio::io_context& executor);
    ~node_status_registry_s();
    node_status_registry_s(const node_status_registry_s&)            = delete;
    node_status_registry_s& operator=(const node_status_registry_s&) = delete;

    // Install before the first publication. Invoked only on the configuration executor.
    void set_callback(callback_t callback);

    // Render thread only. Own the complete typed group; do no JSON work here.
    template <status::registered_contract T>
    void write(const node_status_handle_s& node, T value)
    {
        if (node.active()) {
            enqueue(node, typeid(T), std::make_unique<value_s<T>>(std::move(value)));
        }
    }

    // Render thread, after complete(): publish one frame, coalescing any waiting frames.
    void publish();
    // Stop publication/callbacks during teardown. Queued handlers own their state, never this object.
    void stop() noexcept;

    // Configuration thread (or quiescent startup/tests) only. Reads see the last processed batch.
    void           remove_node(const node_status_handle_s& node);
    nlohmann::json get(std::string_view node_id) const;
    nlohmann::json get_all() const;
};

} // namespace miximus::core
