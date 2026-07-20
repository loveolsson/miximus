#include "render/test_pattern/test_pattern.hpp"

#include "core/app_state.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "render/surface/surface.hpp"
#include "utils/lookup.hpp"
#include "utils/observed_value.hpp"

#include <boost/fiber/future.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string_view>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;

struct request_s
{
    gpu::vec2i_t           dimensions;
    render::test_pattern_e pattern;
    bool                   show_logo;

    bool operator==(const request_s&) const = default;
};

struct generation_s
{
    request_s                                               request;
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
    boost::fibers::future<void>                             worker;
    uint64_t                                                upload_version{};
    bool                                                    submitted{};
};

void generate_pattern(gpu::vec2i_t                          dimensions,
                      render::test_pattern_e                pattern,
                      bool                                  show_logo,
                      gpu::transfer::texture_upload_lease_s upload)
{
    render::surface_s surface(dimensions, static_cast<render::surface_s::rgba_pixel_t*>(upload.ptr()));
    render::render_test_pattern(surface, pattern);
    if (show_logo) {
        render::render_test_pattern_logo(surface);
    }
    upload.submit();
}

class node_impl : public node_i
{
    output_interface_s<gpu::texture_s*> iface_texture_{*this, "texture"};

    utils::observed_value_s<request_s>                      desired_request_;
    std::optional<request_s>                                published_request_;
    std::optional<request_s>                                failed_request_;
    std::optional<generation_s>                             generation_;
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> published_stream_;
    gpu::texture_s*                                         published_texture_{};

    static std::shared_ptr<gpu::transfer::texture_upload_stream_s> create_stream(core::app_state_s* app,
                                                                                 gpu::vec2i_t       dimensions)
    {
        const auto row_stride = sizeof(render::surface_s::rgba_pixel_t) * static_cast<size_t>(dimensions.x);
        const gpu::transfer::texture_transfer_requirements_s requirements{
            .dimensions        = dimensions,
            .format            = gpu::texture_s::format_e::rgba_f16,
            .row_stride        = row_stride,
            .byte_size         = row_stride * static_cast<size_t>(dimensions.y),
            .address_alignment = render::surface_s::DATA_ALIGNMENT,
            .host_access       = gpu::transfer::host_access_e::overwrite,
        };
        return app->texture_upload_service()->create_stream({
            .requirements = requirements,
            .max_slots    = 1,
        });
    }

    void finish_generation()
    {
        if (!generation_.has_value() || !generation_->submitted) {
            return;
        }

        if (generation_->worker.valid()) {
            if (generation_->worker.wait_for(0ms) != boost::fibers::future_status::ready) {
                return;
            }
            try {
                generation_->worker.get();
            } catch (const std::exception& error) {
                getlog("gpu")->error("Test-pattern generation failed: {}", error.what());
                failed_request_ = generation_->request;
                generation_.reset();
                return;
            } catch (...) {
                getlog("gpu")->error("Test-pattern generation failed with an unknown error");
                failed_request_ = generation_->request;
                generation_.reset();
                return;
            }
        }

        auto* texture = generation_->stream->consume_through(generation_->upload_version);
        if (texture == nullptr) {
            return;
        }

        if (desired_request_.has_value() && generation_->request == desired_request_.value()) {
            published_request_ = generation_->request;
            published_stream_  = generation_->stream;
            published_texture_ = texture;
        }
        generation_.reset();
    }

    void begin_generation(core::app_state_s* app)
    {
        if (generation_.has_value() || !desired_request_.has_value() ||
            (published_request_.has_value() && *published_request_ == desired_request_.value()) ||
            (failed_request_.has_value() && *failed_request_ == desired_request_.value())) {
            return;
        }

        generation_.emplace(generation_s{
            .request = desired_request_.value(),
            .stream  = create_stream(app, desired_request_.value().dimensions),
            .worker  = {},
        });
    }

    void submit_generation(core::app_state_s* app)
    {
        if (!generation_.has_value() || generation_->submitted) {
            return;
        }

        auto upload = generation_->stream->try_acquire();
        if (!upload.has_value()) {
            if (generation_->stream->allocation_failed()) {
                getlog("gpu")->error("Unable to allocate test-pattern surface {}x{}",
                                     generation_->request.dimensions.x,
                                     generation_->request.dimensions.y);
                failed_request_ = generation_->request;
                generation_.reset();
            }
            return;
        }

        generation_->upload_version = upload->version();
        auto worker                 = app->thread_pool()->submit(generate_pattern,
                                                 generation_->request.dimensions,
                                                 generation_->request.pattern,
                                                 generation_->request.show_logo,
                                                 std::move(*upload));
        if (!worker.has_value()) {
            return;
        }

        generation_->worker    = std::move(*worker);
        generation_->submitted = true;
    }

  public:
    node_impl() = default;

    node_impl(const node_impl&)            = delete;
    node_impl(node_impl&&)                 = delete;
    node_impl& operator=(const node_impl&) = delete;
    node_impl& operator=(node_impl&&)      = delete;

    ~node_impl() override
    {
        if (generation_.has_value() && generation_->worker.valid()) {
            try {
                generation_->worker.get();
            } catch (...) { // NOLINT(bugprone-empty-catch) -- destructor must not throw
            }
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& state) final
    {
        const auto         resolution = state.get_option<gpu::vec2_t>("resolution", {1920, 1080});
        const gpu::vec2i_t dimensions{
            static_cast<int>(std::round(resolution.x)),
            static_cast<int>(std::round(resolution.y)),
        };
        const request_s request{
            .dimensions = dimensions,
            .pattern    = state.get_enum_option("pattern", render::test_pattern_e::smpte_color_bars),
            .show_logo  = state.get_option<bool>("show_logo"),
        };
        if (desired_request_.observe(request)) {
            failed_request_.reset();
            if (generation_.has_value() && !generation_->submitted) {
                generation_.reset();
            }
        }

        finish_generation();
        begin_generation(app);
        submit_generation(app);

        if (published_stream_) {
            if (auto* texture = published_stream_->consume_latest(); texture != nullptr) {
                published_texture_ = texture;
            }
        }
        iface_texture_.set_value(published_texture_);
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",       "Test pattern"         },
            {"resolution", gpu::vec2_t{1920, 1080}},
            {"pattern",    "smpte_color_bars"     },
            {"show_logo",  false                  },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "resolution") {
            auto result = normalize_option_value<gpu::vec2_t>(value, gpu::vec2_t{1, 1}, gpu::vec2_t{16384, 16384});
            if (result == option_result_e::invalid) {
                return result;
            }

            auto&      x         = value->at(0);
            auto&      y         = value->at(1);
            const auto rounded_x = std::round(x.get<double>());
            const auto rounded_y = std::round(y.get<double>());
            if (x != rounded_x || y != rounded_y) {
                x      = rounded_x;
                y      = rounded_y;
                result = option_result_e::corrected;
            }
            return result;
        }

        if (name == "pattern") {
            if (value == nullptr || !value->is_string()) {
                return option_result_e::invalid;
            }
            return enum_from_string<render::test_pattern_e>(value->get<std::string_view>()).has_value()
                       ? option_result_e::ok
                       : option_result_e::invalid;
        }

        if (name == "show_logo") {
            return normalize_option_value<bool>(value);
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return "test_pattern"; }
};

} // namespace

namespace miximus::nodes::generators {

std::shared_ptr<node_i> create_test_pattern_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::generators
