#include "core/app_state.hpp"
#include "nodes/composite/register.hpp"
#include "nodes/frame_execution.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/option_result.hpp"
#include "nodes/switch/register.hpp"

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
    std::string                       type_;
    std::vector<std::string>*         events_{};
    bool                              demands_execution_{};
    nodes::input_interface_s<double>  source_{*this, "source"};
    nodes::input_interface_s<double>  input_{*this, "input"};
    nodes::output_interface_s<double> output_{*this, "out"};

    void record(std::string_view phase) const { events_->emplace_back(std::string(phase) + ":" + type_); }

  public:
    test_node_s(std::string type, std::vector<std::string>* events, bool demands_execution = false)
        : type_(std::move(type))
        , events_(events)
        , demands_execution_(demands_execution)
    {
    }

    ~test_node_s() override = default;

    test_node_s(const test_node_s&)            = delete;
    test_node_s(test_node_s&&)                 = delete;
    test_node_s& operator=(const test_node_s&) = delete;
    test_node_s& operator=(test_node_s&&)      = delete;

    std::string_view type() const final { return type_; }

    void prepare(core::app_state_s* /*app*/, const nodes::node_state_s& /*state*/, prepare_result_s* result) final
    {
        record("prepare");
        result->demands_execution = demands_execution_;
    }

    void submit(core::app_state_s* app, const nodes::node_map_t& nodes, const nodes::node_state_s& state) final
    {
        record("submit");
        for (const auto* iface :
             {static_cast<const nodes::interface_i*>(&source_), static_cast<const nodes::interface_i*>(&input_)}) {
            iface->submit_connections(app, nodes, state);
        }
    }

    void execute(core::app_state_s* app, const nodes::node_map_t& nodes, const nodes::node_state_s& state) final
    {
        (void)source_.resolve_value(app, nodes, state);
        (void)input_.resolve_value(app, nodes, state);
        output_.set_value(1.0);
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
    for (const auto& [name, _] : record.node->get_interfaces()) {
        record.state.con_map.emplace(name, nodes::con_set_t{});
    }
    nodes->emplace(std::move(id), std::move(record));
}

void add_registered_node(nodes::node_map_t*                  nodes,
                         std::string                         id,
                         std::string_view                    type,
                         const nodes::node_definition_map_t& definitions)
{
    const auto definition = definitions.find(type);
    ASSERT_NE(definition, definitions.end());

    nodes::node_record_s record;
    record.node          = definition->second.constructor();
    record.state.options = record.node->get_default_options();
    record.node->init(id);
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
    std::vector<std::string> events;
    nodes::node_map_t        graph;
    core::app_state_s        app(core::app_state_s::test_state_t{});
    add_node(&graph, "source", &events);
    add_node(&graph, "shared", &events);
    add_node(&graph, "sink_a", &events, true);
    add_node(&graph, "sink_b", &events, true);
    add_node(&graph, "inactive", &events);

    connect(&graph, "source", "shared", "source");
    connect(&graph, "shared", "sink_a", "input");
    connect(&graph, "shared", "sink_b", "input");

    const auto demanding_nodes = nodes::prepare_all_nodes(&app, graph);
    ASSERT_EQ(demanding_nodes.size(), 2);
    for (const auto id : {"source", "shared", "sink_a", "sink_b", "inactive"}) {
        EXPECT_EQ(count_event(events, std::string("prepare:") + id), 1);
    }

    nodes::submit_demanding_nodes(&app, graph, demanding_nodes);
    EXPECT_EQ(app.frame_info.submitted_nodes.size(), 4);
    for (const auto id : {"source", "shared", "sink_a", "sink_b"}) {
        EXPECT_TRUE(app.frame_info.submitted_nodes.contains(id));
    }
    EXPECT_FALSE(app.frame_info.submitted_nodes.contains("inactive"));

    for (const auto id : {"source", "shared", "sink_a", "sink_b"}) {
        EXPECT_EQ(count_event(events, std::string("submit:") + id), 1);
    }
    EXPECT_EQ(count_event(events, "submit:inactive"), 0);

    nodes::execute_demanding_nodes(&app, graph, demanding_nodes);
    EXPECT_EQ(count_event(events, "execute:sink_a"), 1);
    EXPECT_EQ(count_event(events, "execute:sink_b"), 1);

    nodes::complete_all_nodes(&app, graph);
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
    core::app_state_s        app(core::app_state_s::test_state_t{});
    add_node(&graph, "shared", &events);

    EXPECT_TRUE(nodes::execute_node_once(&app, graph, "shared"));
    EXPECT_FALSE(nodes::execute_node_once(&app, graph, "shared"));
    EXPECT_EQ(count_event(events, "execute:shared"), 1);
}

TEST(FrameExecution, SubmitOnceSuppressesSharedAndRepeatedRequests)
{
    std::vector<std::string> events;
    nodes::node_map_t        graph;
    core::app_state_s        app(core::app_state_s::test_state_t{});
    add_node(&graph, "shared", &events);

    EXPECT_TRUE(nodes::submit_node_once(&app, graph, "shared"));
    EXPECT_FALSE(nodes::submit_node_once(&app, graph, "shared"));
    EXPECT_EQ(count_event(events, "submit:shared"), 1);
}

TEST(FrameExecution, SwitchSubmissionUsesOptionRouteOrAllConnectedSelectorRoutes)
{
    std::vector<std::string>     events;
    nodes::node_map_t            graph;
    core::app_state_s            app(core::app_state_s::test_state_t{});
    nodes::node_definition_map_t definitions;
    nodes::switch_nodes::register_nodes(&definitions);

    for (const auto id : {"selector", "a", "b", "c", "d"}) {
        add_node(&graph, id, &events);
    }
    add_registered_node(&graph, "switch", "switch_f64_4", definitions);
    connect(&graph, "a", "switch", "a");
    connect(&graph, "b", "switch", "b");
    connect(&graph, "c", "switch", "c");
    connect(&graph, "d", "switch", "d");
    graph.at("switch").state.options["active"] = 2;

    ASSERT_TRUE(nodes::submit_node_once(&app, graph, "switch"));
    EXPECT_TRUE(app.frame_info.submitted_nodes.contains("b"));
    EXPECT_FALSE(app.frame_info.submitted_nodes.contains("a"));
    EXPECT_FALSE(app.frame_info.submitted_nodes.contains("c"));
    EXPECT_FALSE(app.frame_info.submitted_nodes.contains("d"));

    app.frame_info.submitted_nodes.clear();
    connect(&graph, "selector", "switch", "active");
    ASSERT_TRUE(nodes::submit_node_once(&app, graph, "switch"));
    for (const auto id : {"selector", "a", "b", "c", "d"}) {
        EXPECT_TRUE(app.frame_info.submitted_nodes.contains(id));
    }
}

TEST(FrameExecution, MixSubmissionUsesOptionRouteOrBothConnectedControlRoutes)
{
    std::vector<std::string>     events;
    nodes::node_map_t            graph;
    core::app_state_s            app(core::app_state_s::test_state_t{});
    nodes::node_definition_map_t definitions;
    nodes::composite::register_nodes(&definitions);

    for (const auto id : {"framebuffer", "control", "a", "b"}) {
        add_node(&graph, id, &events);
    }
    add_registered_node(&graph, "mix", "mix_tex_2", definitions);
    connect(&graph, "framebuffer", "mix", "fb_in");
    connect(&graph, "a", "mix", "a");
    connect(&graph, "b", "mix", "b");
    graph.at("mix").state.options["t"] = 0.0;

    ASSERT_TRUE(nodes::submit_node_once(&app, graph, "mix"));
    EXPECT_TRUE(app.frame_info.submitted_nodes.contains("framebuffer"));
    EXPECT_TRUE(app.frame_info.submitted_nodes.contains("a"));
    EXPECT_FALSE(app.frame_info.submitted_nodes.contains("b"));

    app.frame_info.submitted_nodes.clear();
    connect(&graph, "control", "mix", "t");
    ASSERT_TRUE(nodes::submit_node_once(&app, graph, "mix"));
    for (const auto id : {"framebuffer", "control", "a", "b"}) {
        EXPECT_TRUE(app.frame_info.submitted_nodes.contains(id));
    }
}

} // namespace
