#include "nodes/frame_execution.hpp"
#include "nodes/interface.hpp"
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
    std::string                  type_;
    std::vector<std::string>*    events_;
    bool                         demands_execution_;
    nodes::submitted_node_set_t* submitted_nodes_;

    nodes::input_interface_s<double> source_{*this, "source"};
    nodes::input_interface_s<double> input_{*this, "input"};

    void record(std::string_view phase) const { events_->emplace_back(std::string(phase) + ":" + type_); }

  public:
    test_node_s(std::string                  type,
                std::vector<std::string>*    events,
                bool                         demands_execution = false,
                nodes::submitted_node_set_t* submitted_nodes   = nullptr)
        : type_(std::move(type))
        , events_(events)
        , demands_execution_(demands_execution)
        , submitted_nodes_(submitted_nodes)
    {
    }

    ~test_node_s() override = default;

    std::string_view type() const final { return type_; }

    void prepare(core::app_state_s* /*app*/, const nodes::node_state_s& /*state*/, prepare_result_s* result) final
    {
        record("prepare");
        result->demands_execution = demands_execution_;
    }

    void submit(core::app_state_s* app, const nodes::node_map_t& nodes, const nodes::node_state_s& state) final
    {
        record("submit");
        if (submitted_nodes_ == nullptr) {
            return;
        }

        for (const auto* iface :
             {static_cast<const nodes::interface_i*>(&source_), static_cast<const nodes::interface_i*>(&input_)}) {
            for (const auto& connection : iface->connections(state)) {
                nodes::submit_node_once(app, nodes, connection.from_node, *submitted_nodes_);
            }
        }
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

void add_node(nodes::node_map_t*           nodes,
              std::string                  id,
              std::vector<std::string>*    events,
              bool                         demands_execution = false,
              nodes::submitted_node_set_t* submitted_nodes   = nullptr)
{
    nodes::node_record_s record;
    record.node = std::make_shared<test_node_s>(id, events, demands_execution, submitted_nodes);
    for (const auto& [name, _] : record.node->get_interfaces()) {
        record.state.con_map.emplace(name, nodes::con_set_t{});
    }
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

TEST(FrameExecution, PreparesAndCompletesEveryNodeButSubmitsOnlyDemandedClosure)
{
    std::vector<std::string>    events;
    nodes::node_map_t           graph;
    nodes::submitted_node_set_t submitted_nodes;
    add_node(&graph, "source", &events, false, &submitted_nodes);
    add_node(&graph, "shared", &events, false, &submitted_nodes);
    add_node(&graph, "sink_a", &events, true, &submitted_nodes);
    add_node(&graph, "sink_b", &events, true, &submitted_nodes);
    add_node(&graph, "inactive", &events);

    connect(&graph, "source", "shared", "source");
    connect(&graph, "shared", "sink_a", "input");
    connect(&graph, "shared", "sink_b", "input");

    const auto demanding_nodes = nodes::prepare_all_nodes(nullptr, graph);
    ASSERT_EQ(demanding_nodes.size(), 2);
    for (const auto id : {"source", "shared", "sink_a", "sink_b", "inactive"}) {
        EXPECT_EQ(count_event(events, std::string("prepare:") + id), 1);
    }

    for (const auto id : demanding_nodes) {
        nodes::submit_node_once(nullptr, graph, id, submitted_nodes);
    }
    EXPECT_EQ(submitted_nodes.size(), 4);
    for (const auto id : {"source", "shared", "sink_a", "sink_b"}) {
        EXPECT_TRUE(submitted_nodes.contains(id));
    }
    EXPECT_FALSE(submitted_nodes.contains("inactive"));

    for (const auto id : {"source", "shared", "sink_a", "sink_b"}) {
        EXPECT_EQ(count_event(events, std::string("submit:") + id), 1);
    }
    EXPECT_EQ(count_event(events, "submit:inactive"), 0);

    nodes::executed_node_set_t executed_nodes;
    for (const auto id : demanding_nodes) {
        nodes::execute_node_once(nullptr, graph, id, executed_nodes);
    }
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

TEST(FrameExecution, SubmitOnceSuppressesSharedAndRepeatedRequests)
{
    std::vector<std::string> events;
    nodes::node_map_t        graph;
    add_node(&graph, "shared", &events);

    nodes::submitted_node_set_t submitted_nodes;
    EXPECT_TRUE(nodes::submit_node_once(nullptr, graph, "shared", submitted_nodes));
    EXPECT_FALSE(nodes::submit_node_once(nullptr, graph, "shared", submitted_nodes));
    EXPECT_EQ(count_event(events, "submit:shared"), 1);
}

} // namespace
