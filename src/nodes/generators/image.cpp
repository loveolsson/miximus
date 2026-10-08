#include "core/app_state.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "render/image_file.hpp"
#include "utils/failure_shutdown.hpp"

#include <chrono>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace std::chrono_literals;

struct uploaded_image_s
{
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
    gpu::transfer::texture_upload_id_s                      upload_id;
};

uploaded_image_s load_image(gpu::transfer::texture_upload_service_s* uploads, const std::string& path)
{
    const auto image  = render::load_image_file(path);
    const auto pixels = image.pixels();
    auto       stream = uploads->create_stream({
        .host_layout = {.image_dimensions  = {image.width(), image.height()},
                        .pixel_format      = gpu::transfer::host_pixel_format_e::rgba_u8,
                        .row_stride_bytes  = static_cast<size_t>(image.width()) * 4,
                        .buffer_size_bytes = pixels.size(),
                        .memory_access     = gpu::transfer::host_memory_access_e::overwrite},
        .max_slots   = 1,
    });
    // Allocation runs on the transfer worker. Only this background task waits.
    auto upload = stream->acquire_upload_buffer_for(3s);
    if (!upload) {
        throw std::runtime_error("Unable to allocate image upload storage");
    }
    std::memcpy(upload->writable_host_bytes().data(), pixels.data(), pixels.size());
    const auto id = upload->upload_id();
    if (!upload->submit()) {
        throw std::runtime_error("Unable to submit image upload");
    }
    return {.stream = std::move(stream), .upload_id = id};
}

struct generation_s
{
    std::string                         path;
    utils::cpu_task_s<uploaded_image_s> worker;
    std::optional<uploaded_image_s>     upload;
};

class node_impl final : public node_i
{
    output_interface_s<texture_source_info_s> texture_{*this, "texture"};
    std::string                               desired_path_;
    std::optional<std::string>                failed_path_;
    std::optional<std::string>                published_path_;
    std::optional<generation_s>               generation_;
    std::optional<uploaded_image_s>           published_;
    gpu::texture_frame_ptr                    frame_;
    gpu::texture_frame_ptr                    rendered_frame_;

    void finish_generation()
    {
        if (!generation_) {
            return;
        }
        try {
            if (generation_->worker.valid()) {
                if (generation_->worker.wait_for(0ms) != std::future_status::ready) {
                    return;
                }
                generation_->upload = generation_->worker.get();
            }
            if (generation_->path != desired_path_) {
                generation_.reset();
                return;
            }
            auto& uploaded = *generation_->upload;
            auto  frame    = uploaded.stream->select_latest_completed_upload_through(uploaded.upload_id);
            if (!frame) {
                return;
            }
            published_path_ = generation_->path;
            published_      = std::move(uploaded);
            frame_          = std::move(frame);
        } catch (const std::exception& error) {
            getlog("gpu")->error("Image '{}' failed: {}", generation_->path, error.what());
            failed_path_ = generation_->path;
        }
        generation_.reset();
    }

  public:
    node_impl()                            = default;
    node_impl(const node_impl&)            = delete;
    node_impl(node_impl&&)                 = delete;
    node_impl& operator=(const node_impl&) = delete;
    node_impl& operator=(node_impl&&)      = delete;

    ~node_impl() override
    {
        try {
            if (generation_) {
                (void)generation_->worker.cancel();
            }
        } catch (const std::exception& error) {
            utils::request_failure_shutdown("Image task cancellation failed", error.what());
        } catch (...) {
            utils::request_failure_shutdown("Image task cancellation failed");
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& state) final
    {
        const auto path = state.get_option<std::string>("file_path");
        if (path != desired_path_) {
            desired_path_ = path;
            failed_path_.reset();
            if (generation_ && generation_->worker.cancel()) {
                generation_.reset();
            }
            if (path.empty()) {
                published_.reset();
                published_path_.reset();
                frame_.reset();
            }
        }
        finish_generation();
        if (!generation_ && !path.empty() && published_path_ != path && failed_path_ != path) {
            auto task = app->cpu_task_worker()->submit(
                utils::cpu_task_priority_e::background, load_image, app->texture_upload_service(), path);
            if (task) {
                generation_.emplace(generation_s{.path = path, .worker = std::move(*task), .upload = {}});
            }
        }
        rendered_frame_ = frame_;
        texture_.set_value({.texture = rendered_frame_ ? rendered_frame_->texture() : nullptr,
                            .name    = state.get_option_string_view("name")});
    }

    void complete(core::app_state_s* /*app*/) final { rendered_frame_.reset(); }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",      "Image"},
            {"file_path", ""     }
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        return name == "file_path" ? normalize_option_value<std::string_view>(value) : option_result_e::invalid;
    }

    std::string_view type() const final { return "image"; }
};
} // namespace

namespace miximus::nodes::generators {
std::shared_ptr<node_i> create_image_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::generators
