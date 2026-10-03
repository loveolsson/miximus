#include "nodes/composite/source_labels.hpp"

#include "core/app_state.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "logger/logger.hpp"
#include "render/font/font_info.hpp"
#include "render/font/font_instance.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "render/surface/surface.hpp"
#include "utils/cpu_task_worker.hpp"
#include "utils/observed_value.hpp"
#include "utils/string_map.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace miximus::nodes::composite {
namespace {
using namespace std::chrono_literals;
using upload_stream_t = std::shared_ptr<gpu::transfer::texture_upload_stream_s>;

struct label_s
{
    render::pill_label_s               layout;
    upload_stream_t                    stream;
    gpu::transfer::texture_upload_id_s upload_id{};
    gpu::texture_frame_ptr             frame;
    // Includes labels omitted because their text is empty or cannot fit.
    bool finished{};
};

struct batch_s
{
    render::pill_style_s                     style;
    utils::string_map_t<label_s>             labels;
    std::unique_ptr<render::font_instance_s> font;
};

upload_stream_t make_upload_stream(gpu::transfer::texture_upload_service_s* uploads, gpu::vec2i_t dimensions)
{
    const auto stride = sizeof(render::surface_s::pixel_t) * static_cast<size_t>(dimensions.x);
    const gpu::transfer::host_frame_layout_s host_layout{
        .image_dimensions        = dimensions,
        .pixel_format            = gpu::transfer::host_pixel_format_e::rgba_u8,
        .row_stride_bytes        = stride,
        .buffer_size_bytes       = stride * static_cast<size_t>(dimensions.y),
        .address_alignment_bytes = render::surface_s::PREFERRED_DATA_ALIGNMENT,
        .memory_access           = gpu::transfer::host_memory_access_e::read_write,
    };
    return uploads->create_stream({
        .host_layout       = host_layout,
        .max_slots         = 1,
        .generate_mip_maps = false,
    });
}

std::unique_ptr<batch_s> prepare_labels(std::unique_ptr<batch_s>                 batch,
                                        const render::font_variant_s&            font_info,
                                        gpu::transfer::texture_upload_service_s* uploads)
{
    const auto loader = std::make_shared<render::font_loader_s>();
    batch->font       = loader->load_font(&font_info);
    if (!batch->font) {
        throw std::runtime_error("Unable to load source-label font");
    }

    batch->font->set_size(batch->style.font_size);
    for (auto& [name, label] : batch->labels) {
        if (label.finished) {
            continue;
        }
        label.layout = render::make_pill_label(*batch->font, name, batch->style);
        if (label.layout.text.empty()) {
            label.finished = true;
        } else {
            label.stream = make_upload_stream(uploads, label.layout.dimensions);
        }
    }

    return batch;
}

struct label_upload_s
{
    std::string                           name;
    gpu::transfer::texture_upload_lease_s lease;
};

std::unique_ptr<batch_s> paint_labels(std::unique_ptr<batch_s> batch, std::vector<label_upload_s> uploads)
{
    for (auto& upload : uploads) {
        auto&             label = batch->labels.at(upload.name);
        render::surface_s surface(label.layout.dimensions, upload.lease.writable_host_bytes());
        render::render_pill_label(*batch->font, label.layout, surface);
        if (!upload.lease.submit()) {
            throw std::runtime_error("Unable to submit source-label upload");
        }
    }

    return batch;
}
} // namespace

struct source_labels_s::impl_s
{
    utils::cpu_task_s<std::unique_ptr<batch_s>>   task;
    std::unique_ptr<batch_s>                      pending_batch;
    utils::string_map_t<label_s>                  completed_labels;
    std::vector<std::string>                      desired_names;
    utils::observed_value_s<render::pill_style_s> desired_style;
    utils::observed_value_s<uint64_t>             desired_font_version;
    uint64_t                                      generation{};
    uint64_t                                      task_generation{};
    bool                                          needs_prepare{};

    impl_s()                               = default;
    impl_s(const impl_s& other)            = delete;
    impl_s& operator=(const impl_s& other) = delete;
    impl_s(impl_s&& other)                 = delete;
    impl_s& operator=(impl_s&& other)      = delete;
    ~impl_s() { (void)task.cancel(); }

    void discard_pending()
    {
        ++generation;
        if (task.cancel()) {
            task = {};
        }
        pending_batch.reset();
    }

    void collect_completed_work()
    {
        if (task.valid()) {
            if (task.wait_for(0ms) != std::future_status::ready) {
                return;
            }
            try {
                auto result = task.get();
                if (task_generation == generation) {
                    pending_batch = std::move(result);
                }
            } catch (const std::exception& error) {
                getlog("nodes")->error("Source-label rendering failed: {}", error.what());
            }
        }

        if (!pending_batch) {
            return;
        }

        for (auto& [name, label] : pending_batch->labels) {
            if (!label.finished && label.upload_id) {
                label.frame    = label.stream->select_completed_upload(label.upload_id);
                label.finished = label.frame != nullptr;
            }
            if (label.finished) {
                completed_labels.insert_or_assign(name, label);
            }
        }
        if (std::ranges::all_of(pending_batch->labels, [](const auto& entry) { return entry.second.finished; })) {
            pending_batch.reset();
        }
    }

    void schedule_prepare(core::app_state_s* app, const render::pill_style_s& style)
    {
        needs_prepare = false;
        auto font     = app->font_registry()->find_font_variant(render::get_default_font_name(), "Regular");
        if (!font) {
            const auto fonts = app->font_registry()->get_font_names();
            if (fonts.empty()) {
                return;
            }
            const auto variants = app->font_registry()->get_font_variant_names(fonts.front());
            if (variants.empty()) {
                return;
            }
            font = app->font_registry()->find_font_variant(fonts.front(), variants.front());
        }
        if (!font || style.font_size <= 0) {
            return;
        }

        auto batch   = std::make_unique<batch_s>();
        batch->style = style;
        for (const auto& name : desired_names) {
            const auto found = completed_labels.find(name);
            if (found != completed_labels.end()) {
                batch->labels.emplace(name, found->second);
            } else {
                batch->labels.emplace(name, label_s{});
            }
        }

        auto submitted_task = app->cpu_task_worker()->submit(utils::cpu_task_priority_e::background,
                                                             prepare_labels,
                                                             std::move(batch),
                                                             *font,
                                                             app->texture_upload_service());
        if (submitted_task) {
            task            = std::move(*submitted_task);
            task_generation = generation;
        }
    }

    void schedule_paint(core::app_state_s* app)
    {
        if (!pending_batch) {
            return;
        }
        std::vector<label_upload_s> uploads;
        for (auto& [name, label] : pending_batch->labels) {
            if (label.finished || label.upload_id) {
                continue;
            }
            auto upload = label.stream->try_acquire_upload_buffer();
            if (upload) {
                label.upload_id = upload->upload_id();
                uploads.push_back({.name = name, .lease = std::move(*upload)});
            } else if (label.stream->allocation_failed()) {
                getlog("nodes")->error("Unable to allocate source-label surface");
                label.finished = true;
            }
        }
        if (uploads.empty()) {
            return;
        }

        auto submitted_task = app->cpu_task_worker()->submit(
            utils::cpu_task_priority_e::background, paint_labels, std::move(pending_batch), std::move(uploads));
        if (submitted_task) {
            task            = std::move(*submitted_task);
            task_generation = generation;
        }
    }
};

source_labels_s::source_labels_s()
    : impl_(std::make_unique<impl_s>())
{
}
source_labels_s::~source_labels_s() = default;

void source_labels_s::clear()
{
    auto& state = *impl_;
    state.discard_pending();
    state.completed_labels.clear();
    state.desired_names.clear();
    state.needs_prepare = false;
    state.collect_completed_work();
}

void source_labels_s::update(core::app_state_s*                     app,
                             std::span<const texture_source_info_s> sources,
                             const render::pill_style_s&            style)
{
    auto&                         state = *impl_;
    std::vector<std::string_view> names;
    for (const auto& source : sources) {
        if (!source.name.empty()) {
            names.emplace_back(source.name);
        }
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());

    const auto version       = app->font_registry()->get_font_list_version();
    const bool style_changed = state.desired_style.observe(style);
    const bool font_changed  = state.desired_font_version.observe(version);
    if (!std::ranges::equal(names, state.desired_names) || style_changed || font_changed) {
        state.discard_pending();
        if (style_changed || font_changed) {
            state.completed_labels.clear();
        } else {
            std::erase_if(state.completed_labels,
                          [&names](const auto& entry) { return !std::ranges::binary_search(names, entry.first); });
        }
        state.desired_names.assign(names.begin(), names.end());
        state.needs_prepare = !state.desired_names.empty();
    }

    state.collect_completed_work();
    if (state.task.valid()) {
        return;
    }
    if (state.needs_prepare) {
        state.schedule_prepare(app, style);
    } else {
        state.schedule_paint(app);
    }
}

const gpu::texture_s* source_labels_s::find(std::string_view name) const
{
    const auto found = impl_->completed_labels.find(name);
    if (found == impl_->completed_labels.end() || !found->second.frame) {
        return nullptr;
    }
    return found->second.frame->texture();
}

} // namespace miximus::nodes::composite
