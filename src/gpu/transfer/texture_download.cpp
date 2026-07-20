#include "texture_download.hpp"

#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/sync.hpp"
#include "gpu/transfer/detail/backend_factory.hpp"
#include "gpu/transfer/detail/requirements.hpp"
#include "gpu/transfer/detail/transfer_worker.hpp"
#include "logger/logger.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace miximus::gpu::transfer::detail {
namespace {
enum class slot_state_e : uint8_t
{
    free,
    rendering,
    queued,
    ready,
    cpu_reading,
};

enum class task_type_e : uint8_t
{
    allocate,
    download,
    destroy_stream,
};

} // namespace

struct texture_download_slot_s
{
    std::unique_ptr<backend_i> backend;
    std::unique_ptr<texture_s> texture;
    std::unique_ptr<sync_s>    render_sync;
    slot_state_e               state{slot_state_e::free};
    size_t                     reserved_bytes{};
    uint64_t                   tag{};
};

struct texture_download_stream_state_s
{
    std::weak_ptr<texture_download_service_state_s>       service;
    texture_download_desc_s                               desc;
    mutable std::mutex                                    mutex;
    std::vector<std::shared_ptr<texture_download_slot_s>> slots;
    std::deque<std::shared_ptr<texture_download_slot_s>>  free_slots;
    std::deque<std::shared_ptr<texture_download_slot_s>>  ready_slots;
    size_t                                                pending_allocations{};
    size_t                                                active_targets{};
    size_t                                                active_frames{};
    std::chrono::steady_clock::time_point                 retry_allocation_after;
    bool                                                  allocation_failed{};
    bool                                                  active{true};
};

struct task_s
{
    task_type_e                                      type;
    std::shared_ptr<texture_download_stream_state_s> stream;
    std::shared_ptr<texture_download_slot_s>         slot;
};

struct texture_download_service_state_s : transfer_worker_s<texture_download_service_state_s, task_s>
{
    texture_download_service_state_s(context_s* parent, size_t budget)
        : transfer_worker_s(parent, budget)
    {
    }

    void release_slot(texture_download_slot_s& slot)
    {
        if (slot.render_sync) {
            slot.render_sync->gpu_wait();
            slot.render_sync.reset();
        }
        if (slot.texture && slot.backend) {
            (void)slot.backend->begin_texture_use();
            (void)slot.backend->unbind_texture();
        }
        slot.backend.reset();
        slot.texture.reset();
        release_memory(slot.reserved_bytes);
        slot.reserved_bytes = 0;
    }

    void allocate_slot(const std::shared_ptr<texture_download_stream_state_s>& stream)
    {
        size_t reserved{};
        bool   reserved_memory{};
        try {
            reserved = estimate_slot_memory_usage(stream->desc.requirements);
            if (!reserve_memory(reserved)) {
                throw std::bad_alloc();
            }
            reserved_memory = true;

            auto slot            = std::make_shared<texture_download_slot_s>();
            slot->reserved_bytes = reserved;
            slot->texture =
                std::make_unique<texture_s>(stream->desc.requirements.dimensions, stream->desc.requirements.format);
            auto backend =
                create_backend(stream->desc.requirements, backend_i::direction_e::gpu_to_cpu, slot->texture.get());
            slot->backend = std::move(backend.backend);

            const auto actual_reserved = slot_memory_usage(stream->desc.requirements, backend.allocation_bytes);
            if (!resize_memory_reservation(reserved, actual_reserved)) {
                throw std::bad_alloc();
            }
            reserved             = actual_reserved;
            slot->reserved_bytes = actual_reserved;
            context_s::flush();

            const std::scoped_lock lock(stream->mutex);
            --stream->pending_allocations;
            if (!stream->active) {
                release_slot(*slot);
                return;
            }
            stream->allocation_failed      = false;
            stream->retry_allocation_after = {};
            stream->slots.emplace_back(slot);
            stream->free_slots.emplace_back(std::move(slot));
        } catch (const std::exception& error) {
            if (reserved_memory) {
                release_memory(reserved);
            }
            {
                const std::scoped_lock lock(stream->mutex);
                --stream->pending_allocations;
                stream->allocation_failed      = true;
                stream->retry_allocation_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            getlog("gpu")->error("Unable to allocate texture download slot: {}", error.what());
        }
    }

    static void download_slot(const std::shared_ptr<texture_download_stream_state_s>& stream,
                              const std::shared_ptr<texture_download_slot_s>&         slot)
    {
        bool success = true;
        if (slot->render_sync) {
            slot->render_sync->gpu_wait();
            slot->render_sync.reset();
        }
        success = slot->backend->transfer() && success;
        success = slot->backend->wait_for_completion() && success;

        const std::scoped_lock lock(stream->mutex);
        if (!success || !stream->active) {
            slot->state = slot_state_e::free;
            if (stream->active) {
                stream->free_slots.emplace_back(slot);
            }
            return;
        }
        slot->state = slot_state_e::ready;
        stream->ready_slots.emplace_back(slot);
    }

    bool destroy_stream(const std::shared_ptr<texture_download_stream_state_s>& stream)
    {
        std::vector<std::shared_ptr<texture_download_slot_s>> slots;
        {
            const std::scoped_lock lock(stream->mutex);
            const bool             in_flight = std::ranges::any_of(stream->slots, [](const auto& slot) {
                return slot->state == slot_state_e::rendering || slot->state == slot_state_e::queued ||
                       slot->state == slot_state_e::cpu_reading;
            });
            if (stream->pending_allocations != 0 || stream->active_targets != 0 || stream->active_frames != 0 ||
                in_flight) {
                return false;
            }
            slots = std::move(stream->slots);
            stream->free_slots.clear();
            stream->ready_slots.clear();
        }
        for (auto& slot : slots) {
            release_slot(*slot);
        }
        return true;
    }

    bool process_task(task_s& task)
    {
        switch (task.type) {
            case task_type_e::allocate:
                allocate_slot(task.stream);
                return true;
            case task_type_e::download:
                download_slot(task.stream, task.slot);
                return true;
            case task_type_e::destroy_stream:
                return destroy_stream(task.stream);
        }
        return true;
    }
};
} // namespace miximus::gpu::transfer::detail

namespace miximus::gpu::transfer {
namespace {
void return_target(const std::shared_ptr<detail::texture_download_stream_state_s>& stream,
                   const std::shared_ptr<detail::texture_download_slot_s>&         slot)
{
    if (!stream || !slot) {
        return;
    }
    const std::scoped_lock lock(stream->mutex);
    if (slot->state == detail::slot_state_e::rendering) {
        --stream->active_targets;
        slot->state = detail::slot_state_e::free;
        if (stream->active) {
            stream->free_slots.emplace_back(slot);
        }
    }
}

void return_frame(const std::shared_ptr<detail::texture_download_stream_state_s>& stream,
                  const std::shared_ptr<detail::texture_download_slot_s>&         slot)
{
    if (!stream || !slot) {
        return;
    }
    const std::scoped_lock lock(stream->mutex);
    if (slot->state == detail::slot_state_e::cpu_reading) {
        --stream->active_frames;
        slot->state = detail::slot_state_e::free;
        if (stream->active) {
            stream->free_slots.emplace_back(slot);
        }
    }
}
} // namespace

texture_download_target_s::texture_download_target_s(std::shared_ptr<detail::texture_download_stream_state_s> stream,
                                                     std::shared_ptr<detail::texture_download_slot_s>         slot)
    : stream_(std::move(stream))
    , slot_(std::move(slot))
    , framebuffer_(std::make_unique<framebuffer_s>(slot_->texture.get()))
{
}

texture_download_target_s::~texture_download_target_s()
{
    if (!submitted_) {
        return_target(stream_, slot_);
    }
}

texture_download_target_s::texture_download_target_s(texture_download_target_s&& other) noexcept
    : stream_(std::move(other.stream_))
    , slot_(std::move(other.slot_))
    , framebuffer_(std::move(other.framebuffer_))
    , submitted_(std::exchange(other.submitted_, true))
{
}

texture_download_target_s& texture_download_target_s::operator=(texture_download_target_s&& other) noexcept
{
    if (this != &other) {
        if (!submitted_) {
            return_target(stream_, slot_);
        }
        stream_      = std::move(other.stream_);
        slot_        = std::move(other.slot_);
        framebuffer_ = std::move(other.framebuffer_);
        submitted_   = std::exchange(other.submitted_, true);
    }
    return *this;
}

framebuffer_s* texture_download_target_s::framebuffer() const { return framebuffer_.get(); }

void texture_download_target_s::set_tag(uint64_t tag)
{
    if (slot_) {
        slot_->tag = tag;
    }
}

void texture_download_target_s::submit()
{
    if (!stream_ || !slot_ || submitted_) {
        return;
    }
    auto service = stream_->service.lock();
    if (!service) {
        return;
    }
    slot_->render_sync = std::make_unique<sync_s>();
    context_s::flush();
    {
        const std::scoped_lock lock(stream_->mutex);
        if (!stream_->active || slot_->state != detail::slot_state_e::rendering) {
            slot_->render_sync.reset();
            return;
        }
        slot_->state = detail::slot_state_e::queued;
        --stream_->active_targets;
        submitted_ = true;
    }
    service->enqueue({.type = detail::task_type_e::download, .stream = stream_, .slot = slot_});
}

texture_download_frame_s::texture_download_frame_s(std::shared_ptr<detail::texture_download_stream_state_s> stream,
                                                   std::shared_ptr<detail::texture_download_slot_s>         slot)
    : stream_(std::move(stream))
    , slot_(std::move(slot))
{
}

texture_download_frame_s::~texture_download_frame_s() { return_frame(stream_, slot_); }

texture_download_frame_s::texture_download_frame_s(texture_download_frame_s&& other) noexcept
    : stream_(std::move(other.stream_))
    , slot_(std::move(other.slot_))
{
}

texture_download_frame_s& texture_download_frame_s::operator=(texture_download_frame_s&& other) noexcept
{
    if (this != &other) {
        return_frame(stream_, slot_);
        stream_ = std::move(other.stream_);
        slot_   = std::move(other.slot_);
    }
    return *this;
}

std::span<const std::byte> texture_download_frame_s::bytes() const
{
    if (!slot_) {
        return {};
    }
    return {static_cast<const std::byte*>(slot_->backend->data()), slot_->backend->size()};
}

uint64_t texture_download_frame_s::tag() const { return slot_ ? slot_->tag : 0; }

texture_download_stream_s::texture_download_stream_s(std::shared_ptr<detail::texture_download_stream_state_s> state)
    : state_(std::move(state))
{
}

texture_download_stream_s::~texture_download_stream_s()
{
    auto service = state_->service.lock();
    {
        const std::scoped_lock lock(state_->mutex);
        state_->active = false;
    }
    if (service) {
        service->enqueue({.type = detail::task_type_e::destroy_stream, .stream = state_, .slot = {}});
    }
}

std::optional<texture_download_target_s> texture_download_stream_s::try_acquire()
{
    std::shared_ptr<detail::texture_download_slot_s> slot;
    bool                                             allocate{};
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->active) {
            return std::nullopt;
        }
        if (!state_->free_slots.empty()) {
            slot = std::move(state_->free_slots.front());
            state_->free_slots.pop_front();
            slot->state = detail::slot_state_e::rendering;
            slot->tag   = 0;
            ++state_->active_targets;
        } else if (state_->slots.size() + state_->pending_allocations < state_->desc.max_slots &&
                   state_->pending_allocations == 0 &&
                   std::chrono::steady_clock::now() >= state_->retry_allocation_after) {
            ++state_->pending_allocations;
            allocate = true;
        }
    }
    if (allocate) {
        if (auto service = state_->service.lock()) {
            service->enqueue({.type = detail::task_type_e::allocate, .stream = state_, .slot = {}});
        }
    }
    if (!slot) {
        return std::nullopt;
    }
    if (!slot->backend->begin_texture_use()) {
        return_target(state_, slot);
        return std::nullopt;
    }
    return texture_download_target_s(state_, std::move(slot));
}

std::optional<texture_download_frame_s> texture_download_stream_s::try_consume_latest()
{
    std::shared_ptr<detail::texture_download_slot_s> selected;
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->active || state_->ready_slots.empty()) {
            return std::nullopt;
        }
        while (!state_->ready_slots.empty()) {
            if (selected) {
                selected->state = detail::slot_state_e::free;
                state_->free_slots.emplace_back(std::move(selected));
            }
            selected = std::move(state_->ready_slots.front());
            state_->ready_slots.pop_front();
        }
        selected->state = detail::slot_state_e::cpu_reading;
        ++state_->active_frames;
    }
    return texture_download_frame_s(state_, std::move(selected));
}

bool texture_download_stream_s::allocation_failed() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->allocation_failed;
}

texture_download_desc_s texture_download_stream_s::desc() const { return state_->desc; }

texture_download_service_s::texture_download_service_s(context_s* parent, size_t memory_budget)
    : state_(std::make_shared<detail::texture_download_service_state_s>(parent, memory_budget))
{
    state_->start();
}

texture_download_service_s::~texture_download_service_s()
{
    state_->stop();
    state_.reset();
}

std::shared_ptr<texture_download_stream_s> texture_download_service_s::create_stream(texture_download_desc_s desc)
{
    if (desc.max_slots == 0 || desc.requirements.host_access != host_access_e::read_only) {
        throw std::invalid_argument("invalid texture download stream description");
    }
    detail::normalize_requirements(desc.requirements);
    auto stream     = std::make_shared<detail::texture_download_stream_state_s>();
    stream->service = state_;
    stream->desc    = desc;
    return std::shared_ptr<texture_download_stream_s>(new texture_download_stream_s(std::move(stream)));
}

size_t texture_download_service_s::memory_usage() const { return state_->memory_usage(); }

size_t texture_download_service_s::memory_budget() const { return state_->memory_budget(); }

} // namespace miximus::gpu::transfer
