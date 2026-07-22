#include "nodes/frame_execution.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/option_result.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace miximus;

class test_node_s final : public nodes::node_i
{
    std::string               type_;
    std::vector<std::string>* events_;
    bool                      demands_execution_;

    void record(std::string_view phase) const { events_->emplace_back(std::string(phase) + ":" + type_); }

  public:
    test_node_s(std::string type, std::vector<std::string>* events, bool demands_execution = false)
        : type_(std::move(type))
        , events_(events)
        , demands_execution_(demands_execution)
    {
    }

    ~test_node_s() override = default;

    std::string_view type() const final { return type_; }

    void prepare(core::app_state_s* /*app*/, const nodes::node_state_s& /*state*/, prepare_result_s* result) final
    {
        record("prepare");
        result->demands_execution = demands_execution_;
    }

    void
    submit(core::app_state_s* /*app*/, const nodes::node_map_t& /*nodes*/, const nodes::node_state_s& /*state*/) final
    {
        record("submit");
    }

    void
    execute(core::app_state_s* /*app*/, const nodes::node_map_t& /*nodes*/, const nodes::node_state_s& /*state*/) final
    {
        record("execute");
    }

    void complete(core::app_state_s* /*app*/) final { record("complete"); }

    nodes::option_result_e normalize_option(std::string_view /*name*/, nlohmann::json* /*value*/) const final
    {
        return nodes::option_result_e::invalid;
    }
};

void add_node(nodes::node_map_t*        nodes,
              std::string               id,
              std::vector<std::string>* events,
              bool                      demands_execution = false)
{
    nodes::node_record_s record;
    record.node = std::make_shared<test_node_s>(id, events, demands_execution);
    nodes->emplace(std::move(id), std::move(record));
}

void connect(nodes::node_map_t* nodes, std::string_view from, std::string_view to, std::string_view input_name)
{
    nodes::connection_s connection{
        .from_node      = std::string(from),
        .from_interface = "out",
        .to_node        = std::string(to),
        .to_interface   = std::string(input_name),
    };
    const auto node = nodes->find(to);
    ASSERT_NE(node, nodes->end());
    node->second.state.con_map[input_name].emplace_back(std::move(connection));
}

size_t count_event(const std::vector<std::string>& events, std::string_view event)
{
    return static_cast<size_t>(std::ranges::count(events, event));
}

size_t find_active_index(std::span<const nodes::frame_execution_entry_s> entries, std::string_view id)
{
    const auto result = std::ranges::find(entries, id, &nodes::frame_execution_entry_s::id);
    return static_cast<size_t>(std::distance(entries.begin(), result));
}

TEST(FrameExecution, PreparesAndCompletesEveryNodeButSubmitsOnlyDemandedClosure)
{
    std::vector<std::string> events;
    nodes::node_map_t        graph;
    add_node(&graph, "source", &events);
    add_node(&graph, "shared", &events);
    add_node(&graph, "sink_a", &events, true);
    add_node(&graph, "sink_b", &events, true);
    add_node(&graph, "inactive", &events);

    connect(&graph, "source", "shared", "source");
    connect(&graph, "shared", "sink_a", "input");
    connect(&graph, "shared", "sink_b", "input");

    const auto demanding_nodes = nodes::prepare_all_nodes(nullptr, graph);
    ASSERT_EQ(demanding_nodes.size(), 2);
    for (const auto id : {"source", "shared", "sink_a", "sink_b", "inactive"}) {
        EXPECT_EQ(count_event(events, std::string("prepare:") + id), 1);
    }

    const nodes::frame_execution_plan_s plan(graph, demanding_nodes);
    const auto                          active = plan.active_nodes();
    ASSERT_EQ(active.size(), 4);
    EXPECT_LT(find_active_index(active, "source"), find_active_index(active, "shared"));
    EXPECT_LT(find_active_index(active, "shared"), find_active_index(active, "sink_a"));
    EXPECT_LT(find_active_index(active, "shared"), find_active_index(active, "sink_b"));
    EXPECT_EQ(find_active_index(active, "inactive"), active.size());

    plan.submit(nullptr);
    for (const auto id : {"source", "shared", "sink_a", "sink_b"}) {
        EXPECT_EQ(count_event(events, std::string("submit:") + id), 1);
    }
    EXPECT_EQ(count_event(events, "submit:inactive"), 0);

    nodes::executed_node_set_t executed_nodes;
    plan.execute(nullptr, executed_nodes);
    EXPECT_EQ(count_event(events, "execute:sink_a"), 1);
    EXPECT_EQ(count_event(events, "execute:sink_b"), 1);

    nodes::complete_all_nodes(nullptr, graph);
    for (const auto id : {"source", "shared", "sink_a", "sink_b", "inactive"}) {
        EXPECT_EQ(count_event(events, std::string("complete:") + id), 1);
    }

    const auto first_execute =
        std::ranges::find_if(events, [](const std::string& event) { return event.starts_with("execute:"); });
    const auto last_submit = std::ranges::find_if(
        events.rbegin(), events.rend(), [](const std::string& event) { return event.starts_with("submit:"); });
    ASSERT_NE(first_execute, events.end());
    ASSERT_NE(last_submit, events.rend());
    EXPECT_LT(std::distance(events.begin(), last_submit.base() - 1), std::distance(events.begin(), first_execute));
}

TEST(FrameExecution, ExecuteOnceSuppressesSharedAndRepeatedRequests)
{
    std::vector<std::string> events;
    nodes::node_map_t        graph;
    add_node(&graph, "shared", &events);

    nodes::executed_node_set_t executed_nodes;
    EXPECT_TRUE(nodes::execute_node_once(nullptr, graph, "shared", executed_nodes));
    EXPECT_FALSE(nodes::execute_node_once(nullptr, graph, "shared", executed_nodes));
    EXPECT_EQ(count_event(events, "execute:shared"), 1);
}
} // namespace
