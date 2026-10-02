#include "core/app_state.hpp"
#include "gpu/texture.hpp"
#include "nodes/composite/register.hpp"
#include "nodes/frame_execution.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/option_result.hpp"
#include "nodes/switch/register.hpp"
#include "nodes/utils/register.hpp"

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
            miximus::nodes::interface_i::submit_dependencies(app, nodes, iface->connections(state));
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

void connect(nodes::node_map_t* nodes,
             std::string_view   from,
             std::string_view   to,
             std::string_view   input_name,
             std::string_view   output_name = "out")
{
    connection_s connection{
        .from_node      = std::string(from),
        .from_interface = std::string(output_name),
        .to_node        = std::string(to),
        .to_interface   = std::string(input_name),
    };
    const auto node = nodes->find(to);
    ASSERT_NE(node, nodes->end());
    node->second.state.con_map[input_name].emplace_back(std::move(connection));
}

template <typename T>
class source_node_s final : public nodes::node_i
{
    nodes::output_interface_s<T> output_{*this, "out"};
    T                            source_;

  public:
    explicit source_node_s(T source)
        : source_(std::move(source))
    {
    }
    std::string_view type() const final { return "test_source"; }
    void execute(core::app_state_s* /*app*/, const nodes::node_map_t& /*nodes*/, const nodes::node_state_s& state) final
    {
        output_.set_value({.texture = source_.texture, .name = state.get_option_string_view("name")});
    }
    nodes::option_result_e normalize_option(std::string_view /*name*/, nlohmann::json* /*value*/) const final
    {
        return nodes::option_result_e::invalid;
    }
};

template <typename T>
void add_source(nodes::node_map_t* graph, std::string id, T source, std::string_view name)
{
    nodes::node_record_s record;
    record.node = std::make_shared<source_node_s<T>>(std::move(source));
    record.node->init(id);
    record.state.options = {
        {"name", name}
    };
    graph->emplace(std::move(id), std::move(record));
}

template <typename T>
T read_output(const nodes::node_map_t& graph, std::string_view id, std::string_view port)
{
    const auto* output =
        dynamic_cast<const nodes::output_interface_s<T>*>(graph.find(id)->second.node->find_interface(port));
    EXPECT_NE(output, nullptr);
    return output ? output->get_value() : T{};
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

TEST(FrameExecution, TexturePortsPreserveReadOnlyAndWritableContracts)
{
    std::vector<std::string>                                    events;
    test_node_s                                                 owner("owner", &events, false);
    nodes::input_interface_s<nodes::texture_source_info_s>      sampled_input(owner, "sampled_input");
    nodes::input_interface_s<nodes::framebuffer_source_info_s>  writable_input(owner, "writable_input");
    nodes::output_interface_s<nodes::texture_source_info_s>     sampled_output(owner, "sampled_output");
    nodes::output_interface_s<nodes::framebuffer_source_info_s> writable_output(owner, "writable_output");
    gpu::texture_s                                              resource;
    sampled_output.set_value({.texture = &resource, .name = std::string_view("Camera")});
    writable_output.set_value({.texture = &resource, .name = std::string_view("Program")});
    EXPECT_EQ(sampled_output.type(), nodes::interface_type_e::texture);
    EXPECT_EQ(writable_output.type(), nodes::interface_type_e::framebuffer);
    EXPECT_TRUE(sampled_input.accepts(sampled_output.type()));
    EXPECT_TRUE(sampled_input.accepts(writable_output.type()));
    EXPECT_TRUE(writable_input.accepts(writable_output.type()));
    EXPECT_FALSE(writable_input.accepts(sampled_output.type()));
    EXPECT_EQ(sampled_input.cast_iface_to_value(&sampled_output, {}).texture, &resource);
    EXPECT_EQ(sampled_input.cast_iface_to_value(&writable_output, {}).texture, &resource);
    EXPECT_EQ(writable_input.cast_iface_to_value(&writable_output, {}).texture, &resource);
    EXPECT_EQ(writable_input.cast_iface_to_value(&sampled_output, {}).texture, nullptr);
}

template <typename T>
void check_rename(std::string_view type, std::string_view input, std::string_view output)
{
    nodes::node_map_t            graph;
    core::app_state_s            app(core::app_state_s::test_state_t{});
    nodes::node_definition_map_t definitions;
    nodes::utils::register_nodes(&definitions);
    gpu::texture_s texture;
    add_source(&graph, "source", T{.texture = &texture, .name = {}}, "Camera");
    add_registered_node(&graph, "rename", type, definitions);
    connect(&graph, "source", "rename", input);
    auto&             state     = graph.at("rename").state;
    const auto&       node      = graph.at("rename").node;
    const std::string long_name = "Kamera — " + std::string(100, 'x');
    for (const auto& name : {std::string("01:23:45:12"), long_name, std::string{}}) {
        ASSERT_EQ(node->set_options(state.options,
                                    {
                                        {"source_name", name}
        })
                      .error,
                  error_e::no_error);
        app.frame_info.executed_nodes.clear();
        ASSERT_TRUE(nodes::execute_node_once(&app, graph, "rename"));
        const auto renamed  = read_output<T>(graph, "rename", output);
        const auto original = read_output<T>(graph, "source", "out");
        EXPECT_EQ(renamed.texture, &texture);
        EXPECT_EQ(renamed.name.view(), name);
        EXPECT_EQ(original.texture, &texture);
        EXPECT_EQ(original.name.view(), "Camera");
    }
    const auto options = state.options;
    EXPECT_NE(node->set_options(state.options,
                                {
                                    {"source_name", 42}
    })
                  .error,
              error_e::no_error);
    EXPECT_EQ(state.options, options);
}

TEST(FrameExecution, RenameCopiesMetadataWithoutMutatingTheSource)
{
    check_rename<nodes::texture_source_info_s>("set_texture_name", "tex_in", "tex_out");
    check_rename<nodes::framebuffer_source_info_s>("set_framebuffer_name", "fb_in", "fb_out");
}

TEST(FrameExecution, TextureBranchesAndSwitchesPreserveNamesAcrossFrames)
{
    nodes::node_map_t            graph;
    core::app_state_s            app(core::app_state_s::test_state_t{});
    nodes::node_definition_map_t definitions;
    nodes::utils::register_nodes(&definitions);
    nodes::switch_nodes::register_nodes(&definitions);
    gpu::texture_s texture;
    add_source(&graph, "source", nodes::texture_source_info_s{.texture = &texture, .name = {}}, "Camera");
    add_registered_node(&graph, "rename", "set_texture_name", definitions);
    add_registered_node(&graph, "switch", "switch_tex_4", definitions);
    connect(&graph, "source", "rename", "tex_in");
    connect(&graph, "source", "switch", "a");
    connect(&graph, "rename", "switch", "b", "tex_out");
    graph.at("rename").state.options["source_name"] = "Preview";
    for (const auto active : {1, 2, 1}) {
        graph.at("switch").state.options["active"] = active;
        app.frame_info.executed_nodes.clear();
        ASSERT_TRUE(nodes::execute_node_once(&app, graph, "switch"));
        const auto result = read_output<nodes::texture_source_info_s>(graph, "switch", "tex");
        EXPECT_EQ(result.texture, &texture);
        EXPECT_EQ(result.name.view(), active == 1 ? "Camera" : "Preview");
        const auto upstream = read_output<nodes::texture_source_info_s>(
            graph, active == 1 ? "source" : "rename", active == 1 ? "out" : "tex_out");
        EXPECT_TRUE(result.name.is_borrowed());
        EXPECT_EQ(result.name.view().data(), upstream.name.view().data());
    }
    graph.at("source").state.options["name"] = "Camera renamed";
    app.frame_info.executed_nodes.clear();
    ASSERT_TRUE(nodes::execute_node_once(&app, graph, "switch"));
    EXPECT_EQ(read_output<nodes::texture_source_info_s>(graph, "switch", "tex").name.view(), "Camera renamed");
}

TEST(FrameExecution, FramebufferConversionCopiesNamesAndRetainsConstness)
{
    std::vector<std::string>                                    events;
    test_node_s                                                 owner("owner", &events, false);
    nodes::output_interface_s<nodes::framebuffer_source_info_s> output(owner, "fb");
    gpu::texture_s                                              texture;
    output.set_value({.texture = &texture, .name = std::string_view("Program")});
    auto converted = nodes::input_interface_s<nodes::texture_source_info_s>::cast_iface_to_value(&output, {});
    EXPECT_EQ(converted.texture, &texture);
    EXPECT_EQ(converted.name.view(), "Program");
    EXPECT_TRUE(converted.name.is_borrowed());
    EXPECT_EQ(converted.name.view().data(), output.get_value().name.view().data());
    const nodes::texture_source_info_s renamed{.texture = converted.texture, .name = std::string_view("Preview")};
    EXPECT_EQ(renamed.name.view(), "Preview");
    EXPECT_EQ(output.get_value().name.view(), "Program");
    const auto explicit_conversion = output.get_value().as_texture();
    EXPECT_EQ(explicit_conversion.texture, &texture);
    EXPECT_EQ(explicit_conversion.name.view(), "Program");
}

TEST(FrameExecution, MissingTextureKeepsItsNameUnlessUsingAFallbackImage)
{
    std::vector<std::string>                                    events;
    test_node_s                                                 owner("owner", &events, false);
    nodes::output_interface_s<nodes::texture_source_info_s>     texture_output(owner, "tex");
    nodes::output_interface_s<nodes::framebuffer_source_info_s> framebuffer_output(owner, "fb");
    texture_output.set_value({.texture = nullptr, .name = std::string_view("Camera")});
    framebuffer_output.set_value({.texture = nullptr, .name = std::string_view("Program")});
    using input_t = nodes::input_interface_s<nodes::texture_source_info_s>;
    EXPECT_EQ(input_t::cast_iface_to_value(&texture_output, {}).name.view(), "Camera");
    EXPECT_EQ(input_t::cast_iface_to_value(&framebuffer_output, {}).name.view(), "Program");
    gpu::texture_s                     texture;
    const nodes::texture_source_info_s fallback{.texture = &texture, .name = std::string_view("Fallback")};
    for (const nodes::interface_i* output : {static_cast<const nodes::interface_i*>(&texture_output),
                                             static_cast<const nodes::interface_i*>(&framebuffer_output)}) {
        const auto result = input_t::cast_iface_to_value(output, fallback);
        EXPECT_EQ(result.texture, &texture);
        EXPECT_EQ(result.name.view(), "Fallback");
    }
}

TEST(FrameExecution, EmptyFramebufferPassThroughPreservesMetadata)
{
    nodes::node_map_t            graph;
    core::app_state_s            app(core::app_state_s::test_state_t{});
    nodes::node_definition_map_t definitions;
    nodes::utils::register_nodes(&definitions);
    nodes::composite::register_nodes(&definitions);
    add_source(&graph, "source", nodes::framebuffer_source_info_s{}, "Program");
    for (const auto type : {"draw_box", "mix_tex_2", "infinite_multiviewer"}) {
        add_registered_node(&graph, type, type, definitions);
        connect(&graph, "source", type, "fb_in");
        ASSERT_TRUE(nodes::execute_node_once(&app, graph, type));
        const auto result = read_output<nodes::framebuffer_source_info_s>(graph, type, "fb_out");
        EXPECT_EQ(result.texture, nullptr);
        EXPECT_EQ(result.name.view(), "Program");
    }
    add_registered_node(&graph, "adapter", "framebuffer_to_texture", definitions);
    connect(&graph, "source", "adapter", "fb");
    ASSERT_TRUE(nodes::execute_node_once(&app, graph, "adapter"));
    const auto result = read_output<nodes::texture_source_info_s>(graph, "adapter", "tex");
    EXPECT_EQ(result.texture, nullptr);
    EXPECT_EQ(result.name.view(), "Program");
}

TEST(FrameExecution, CompletionClearsImageOutputsAndPreservesValueOutputs)
{
    nodes::node_map_t            graph;
    core::app_state_s            app(core::app_state_s::test_state_t{});
    nodes::node_definition_map_t definitions;
    nodes::switch_nodes::register_nodes(&definitions);
    gpu::texture_s texture;
    add_source(&graph, "source", nodes::texture_source_info_s{.texture = &texture, .name = {}}, std::string(128, 'x'));
    add_source(&graph, "framebuffer", nodes::framebuffer_source_info_s{.texture = &texture, .name = {}}, "Program");
    add_registered_node(&graph, "switch", "switch_tex_4", definitions);
    connect(&graph, "source", "switch", "a");
    auto&                                  owner = *graph.at("framebuffer").node;
    nodes::output_interface_s<double>      number(owner, "number");
    nodes::output_interface_s<gpu::vec2_t> vector(owner, "vector");
    nodes::output_interface_s<gpu::rect_s> rectangle(owner, "rectangle");
    number.set_value(42);
    vector.set_value({3, 4});
    rectangle.set_value({
        .pos = {1, 2},
          .size = {3, 4}
    });
    ASSERT_TRUE(nodes::execute_node_once(&app, graph, "switch"));
    ASSERT_TRUE(nodes::execute_node_once(&app, graph, "framebuffer"));
    EXPECT_EQ(read_output<nodes::texture_source_info_s>(graph, "switch", "tex").texture, &texture);
    nodes::complete_all_nodes(&app, graph);
    EXPECT_EQ(read_output<nodes::texture_source_info_s>(graph, "source", "out").texture, nullptr);
    const auto framebuffer = read_output<nodes::framebuffer_source_info_s>(graph, "framebuffer", "out");
    EXPECT_EQ(framebuffer.texture, nullptr);
    EXPECT_TRUE(framebuffer.name.empty());
    EXPECT_EQ(number.get_value(), 42);
    EXPECT_EQ(vector.get_value(), gpu::vec2_t(3, 4));
    EXPECT_EQ(rectangle.get_value().pos, gpu::vec2_t(1, 2));
    EXPECT_EQ(rectangle.get_value().size, gpu::vec2_t(3, 4));

    // The downstream output must no longer borrow the removed source's name.
    graph.erase("source");
    const auto cleared = read_output<nodes::texture_source_info_s>(graph, "switch", "tex");
    EXPECT_EQ(cleared.texture, nullptr);
    EXPECT_TRUE(cleared.name.empty());

    add_source(&graph, "source", nodes::texture_source_info_s{.texture = &texture, .name = {}}, "Next frame");
    app.frame_info.executed_nodes.clear();
    ASSERT_TRUE(nodes::execute_node_once(&app, graph, "switch"));
    const auto refreshed = read_output<nodes::texture_source_info_s>(graph, "switch", "tex");
    EXPECT_EQ(refreshed.texture, &texture);
    EXPECT_EQ(refreshed.name.view(), "Next frame");
    nodes::complete_all_nodes(&app, graph);
}

TEST(FrameExecution, RenamePublishesEmptyOutputForMissingImage)
{
    nodes::node_map_t            graph;
    core::app_state_s            app(core::app_state_s::test_state_t{});
    nodes::node_definition_map_t definitions;
    nodes::utils::register_nodes(&definitions);
    add_source(&graph, "texture", nodes::texture_source_info_s{}, "Missing camera");
    add_source(&graph, "framebuffer", nodes::framebuffer_source_info_s{}, "Missing program");
    add_registered_node(&graph, "texture-name", "set_texture_name", definitions);
    add_registered_node(&graph, "framebuffer-name", "set_framebuffer_name", definitions);
    connect(&graph, "texture", "texture-name", "tex_in");
    connect(&graph, "framebuffer", "framebuffer-name", "fb_in");
    graph.at("texture-name").state.options["source_name"]     = "Camera";
    graph.at("framebuffer-name").state.options["source_name"] = "Program";
    ASSERT_TRUE(nodes::execute_node_once(&app, graph, "texture-name"));
    ASSERT_TRUE(nodes::execute_node_once(&app, graph, "framebuffer-name"));
    const auto texture     = read_output<nodes::texture_source_info_s>(graph, "texture-name", "tex_out");
    const auto framebuffer = read_output<nodes::framebuffer_source_info_s>(graph, "framebuffer-name", "fb_out");
    EXPECT_EQ(texture.texture, nullptr);
    EXPECT_TRUE(texture.name.empty());
    EXPECT_EQ(framebuffer.texture, nullptr);
    EXPECT_TRUE(framebuffer.name.empty());
}

TEST(FrameExecution, AbandonedFrameReleasesOutputLeasesWithoutPublication)
{
    core::app_state_s  app(core::app_state_s::test_state_t{});
    std::weak_ptr<int> retained;
    bool               published = false;
    {
        const core::app_state_s::frame_scope_s frame(app);
        auto                                   lease = std::make_shared<int>(1);
        retained                                     = lease;
        app.defer_output([lease, &published](const gpu::completion_s&) { published = true; });
    }
    EXPECT_TRUE(retained.expired());
    EXPECT_FALSE(published);
}

} // namespace
