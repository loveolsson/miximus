#include "texture_upload.hpp"

#include "gpu/context.hpp"
#include "gpu/transfer/detail/texture_transfer_backend_factory.hpp"
#include "gpu/transfer/detail/transfer_layout.hpp"
#include "gpu/transfer/detail/transfer_worker.hpp"
#include "logger/logger.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
    cpu_writing,
    queued,
    ready,
    current,
    reclaim,
};

enum class task_type_e : uint8_t
{
    allocate,
    upload,
    reclaim,
    destroy_stream,
};

} // namespace

struct texture_upload_slot_s
{
    std::unique_ptr<texture_transfer_backend_i> transfer_backend;
    texture_frame_ptr                           frame;
    slot_state_e                                state{slot_state_e::free};
    size_t                                      reserved_bytes{};
    bool                                        gl_has_texture_access{};
    bool                                        lease_released{true};
    bool                                        discard_requested{};
    texture_upload_id_s                         upload_id{};
};

struct texture_upload_stream_state_s
{
    std::weak_ptr<texture_upload_service_state_s>       service;
    texture_upload_config_s                             config;
    mutable std::mutex                                  mutex;
    std::condition_variable                             slot_cv;
    std::condition_variable                             completion_cv;
    std::vector<std::shared_ptr<texture_upload_slot_s>> slots;
    std::deque<std::shared_ptr<texture_upload_slot_s>>  free_slots;
    std::deque<std::shared_ptr<texture_upload_slot_s>>  ready_slots;
    std::shared_ptr<texture_upload_slot_s>              current_slot;
    size_t                                              pending_allocations{};
    size_t                                              active_leases{};
    uint64_t                                            next_upload_id_value{};
    texture_upload_id_s                                 retained_upload_id{};
    std::chrono::steady_clock::time_point               retry_allocation_after;
    bool                                                allocation_failed{};
    bool                                                active{true};
};

struct task_s
{
    task_type_e                                    type;
    std::shared_ptr<texture_upload_stream_state_s> stream;
    std::shared_ptr<texture_upload_slot_s>         slot;
};

struct texture_upload_service_state_s : transfer_worker_s<texture_upload_service_state_s, task_s>
{
    texture_upload_service_state_s(context_s* parent, size_t budget)
        : transfer_worker_s(parent, budget)
    {
    }

    void release_slot_resources(texture_upload_slot_s& slot)
    {
        if (slot.frame && slot.transfer_backend) {
            if (slot.gl_has_texture_access) {
                slot.transfer_backend->release_texture_from_gl();
                slot.gl_has_texture_access = false;
            }
            slot.transfer_backend->unregister_texture();
        }
        slot.transfer_backend.reset();
        slot.frame.reset();
        release_memory(slot.reserved_bytes);
        slot.reserved_bytes = 0;
    }

    void allocate_slot(const std::shared_ptr<texture_upload_stream_state_s>& stream)
    {
        size_t reserved_bytes  = 0;
        bool   memory_reserved = false;
        try {
            reserved_bytes = estimate_slot_memory_usage(stream->config.transfer_layout);
            if (!reserve_memory(reserved_bytes)) {
                throw std::bad_alloc();
            }
            memory_reserved = true;

            auto slot              = std::make_shared<texture_upload_slot_s>();
            slot->reserved_bytes   = reserved_bytes;
            slot->frame            = std::make_shared<texture_frame_s>(stream->config.transfer_layout.dimensions,
                                                            stream->config.transfer_layout.pixel_format);
            auto transfer_backend  = create_texture_transfer_backend(stream->config.transfer_layout,
                                                                    texture_transfer_backend_i::direction_e::cpu_to_gpu,
                                                                    slot->frame->texture());
            slot->transfer_backend = std::move(transfer_backend.transfer_backend);

            const auto actual_reserved =
                slot_memory_usage(stream->config.transfer_layout, transfer_backend.backend_allocation_bytes);
            if (!resize_memory_reservation(reserved_bytes, actual_reserved)) {
                throw std::bad_alloc();
            }
            reserved_bytes       = actual_reserved;
            slot->reserved_bytes = actual_reserved;

            const std::scoped_lock lock(stream->mutex);
            --stream->pending_allocations;
            if (!stream->active) {
                release_slot_resources(*slot);
                return;
            }
            stream->allocation_failed      = false;
            stream->retry_allocation_after = {};
            stream->slots.emplace_back(slot);
            stream->free_slots.emplace_back(std::move(slot));
            stream->slot_cv.notify_all();
        } catch (const std::exception& error) {
            if (memory_reserved) {
                release_memory(reserved_bytes);
            }
            {
                const std::scoped_lock lock(stream->mutex);
                --stream->pending_allocations;
                stream->allocation_failed      = true;
                stream->retry_allocation_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            stream->slot_cv.notify_all();
            getlog("gpu")->error("Unable to allocate texture upload slot: {}", error.what());
        }
    }

    void upload_slot(const std::shared_ptr<texture_upload_stream_state_s>& stream,
                     const std::shared_ptr<texture_upload_slot_s>&         slot)
    {
        {
            const std::scoped_lock lock(stream->mutex);
            if (!stream->active) {
                slot->state = slot_state_e::reclaim;
                enqueue({.type = task_type_e::reclaim, .stream = stream, .slot = slot});
                return;
            }
        }

        bool success = true;
        if (slot->gl_has_texture_access) {
            success                     = slot->transfer_backend->release_texture_from_gl();
            slot->gl_has_texture_access = false;
        }
        success                     = slot->transfer_backend->submit_transfer() && success;
        success                     = slot->transfer_backend->wait_for_transfer_completion() && success;
        slot->gl_has_texture_access = success;

        if (success && stream->config.generate_mip_maps) {
            slot->frame->texture()->generate_mip_maps();
        }

        if (success) {
            slot->frame->publish_upload_ready_fence();
        }

        const std::scoped_lock lock(stream->mutex);
        if (!success || !stream->active || slot->discard_requested) {
            slot->state = slot_state_e::reclaim;
            stream->completion_cv.notify_all();
            enqueue({.type = task_type_e::reclaim, .stream = stream, .slot = slot});
            return;
        }
        slot->state = slot_state_e::ready;
        stream->ready_slots.emplace_back(slot);
        stream->completion_cv.notify_all();
    }

    static bool reclaim_slot(const std::shared_ptr<texture_upload_stream_state_s>& stream,
                             const std::shared_ptr<texture_upload_slot_s>&         slot)
    {
        const std::scoped_lock lock(stream->mutex);
        if (!slot->lease_released) {
            return false;
        }
        if (!slot->frame->wait_for_render_release_on_worker()) {
            return false;
        }
        if (!stream->active) {
            slot->state = slot_state_e::free;
            return true;
        }
        slot->state = slot_state_e::free;
        stream->free_slots.emplace_back(slot);
        stream->slot_cv.notify_one();
        return true;
    }

    bool destroy_stream(const std::shared_ptr<texture_upload_stream_state_s>& stream)
    {
        std::vector<std::shared_ptr<texture_upload_slot_s>> slots;
        {
            const std::scoped_lock lock(stream->mutex);
            const bool             has_in_flight_slot = std::ranges::any_of(stream->slots, [](const auto& slot) {
                return slot->state == slot_state_e::cpu_writing || slot->state == slot_state_e::queued ||
                       slot->state == slot_state_e::reclaim;
            });
            if (stream->pending_allocations != 0 || stream->active_leases != 0 || has_in_flight_slot) {
                return false;
            }
            slots = std::move(stream->slots);
            stream->free_slots.clear();
            stream->ready_slots.clear();
            stream->current_slot.reset();
        }

        for (auto& slot : slots) {
            release_slot_resources(*slot);
        }
        return true;
    }

    bool process_task(task_s& task)
    {
        switch (task.type) {
            case task_type_e::allocate:
                allocate_slot(task.stream);
                return true;
            case task_type_e::upload:
                upload_slot(task.stream, task.slot);
                return true;
            case task_type_e::reclaim:
                return reclaim_slot(task.stream, task.slot);
            case task_type_e::destroy_stream:
                return destroy_stream(task.stream);
        }
        return true;
    }
};
} // namespace miximus::gpu::transfer::detail

namespace miximus::gpu::transfer {
namespace {
std::shared_ptr<detail::texture_upload_slot_s>
retire_current_slot(const std::shared_ptr<detail::texture_upload_stream_state_s>& stream)
{
    if (!stream->current_slot) {
        return {};
    }

    auto slot   = std::move(stream->current_slot);
    slot->state = detail::slot_state_e::reclaim;
    return slot;
}

void return_unsubmitted_lease(const std::shared_ptr<detail::texture_upload_stream_state_s>& stream,
                              const std::shared_ptr<detail::texture_upload_slot_s>&         slot)
{
    if (!stream || !slot) {
        return;
    }
    const std::scoped_lock lock(stream->mutex);
    if (slot->state == detail::slot_state_e::cpu_writing) {
        --stream->active_leases;
        slot->lease_released = true;
        slot->state          = detail::slot_state_e::free;
        if (stream->active) {
            stream->free_slots.emplace_back(slot);
            stream->slot_cv.notify_one();
        }
    }
}

void release_submitted_lease(const std::shared_ptr<detail::texture_upload_stream_state_s>& stream,
                             const std::shared_ptr<detail::texture_upload_slot_s>&         slot)
{
    if (!stream || !slot) {
        return;
    }
    const std::scoped_lock lock(stream->mutex);
    if (!slot->lease_released) {
        --stream->active_leases;
        slot->lease_released = true;
    }
}
} // namespace

texture_upload_lease_s::texture_upload_lease_s(std::shared_ptr<detail::texture_upload_stream_state_s> stream,
                                               std::shared_ptr<detail::texture_upload_slot_s>         slot)
    : stream_(std::move(stream))
    , slot_(std::move(slot))
{
}

texture_upload_lease_s::~texture_upload_lease_s()
{
    if (submitted_) {
        release_submitted_lease(stream_, slot_);
    } else {
        return_unsubmitted_lease(stream_, slot_);
    }
}

texture_upload_lease_s::texture_upload_lease_s(texture_upload_lease_s&& other) noexcept
    : stream_(std::move(other.stream_))
    , slot_(std::move(other.slot_))
    , submitted_(std::exchange(other.submitted_, true))
{
}

texture_upload_lease_s& texture_upload_lease_s::operator=(texture_upload_lease_s&& other) noexcept
{
    if (this != &other) {
        if (!submitted_) {
            return_unsubmitted_lease(stream_, slot_);
        } else {
            release_submitted_lease(stream_, slot_);
        }
        stream_    = std::move(other.stream_);
        slot_      = std::move(other.slot_);
        submitted_ = std::exchange(other.submitted_, true);
    }
    return *this;
}

std::span<std::byte> texture_upload_lease_s::writable_host_bytes() const noexcept
{
    if (!slot_) {
        return {};
    }
    return {static_cast<std::byte*>(slot_->transfer_backend->host_memory()),
            slot_->transfer_backend->host_buffer_size_bytes()};
}

texture_upload_id_s texture_upload_lease_s::upload_id() const noexcept
{
    return slot_ ? slot_->upload_id : texture_upload_id_s{};
}

bool texture_upload_lease_s::submit()
{
    if (!stream_ || !slot_ || submitted_) {
        return false;
    }
    auto service = stream_->service.lock();
    if (!service) {
        return false;
    }
    {
        const std::scoped_lock lock(stream_->mutex);
        if (!stream_->active || slot_->state != detail::slot_state_e::cpu_writing) {
            return false;
        }
        slot_->state = detail::slot_state_e::queued;
        submitted_   = true;
    }
    service->enqueue({.type = detail::task_type_e::upload, .stream = stream_, .slot = slot_});
    return true;
}

texture_upload_stream_s::texture_upload_stream_s(std::shared_ptr<detail::texture_upload_stream_state_s> state)
    : state_(std::move(state))
{
}

texture_upload_stream_s::~texture_upload_stream_s()
{
    auto                                           service = state_->service.lock();
    std::shared_ptr<detail::texture_upload_slot_s> current;
    {
        const std::scoped_lock lock(state_->mutex);
        state_->active = false;
        current        = retire_current_slot(state_);
        state_->completion_cv.notify_all();
    }
    if (service) {
        if (current) {
            service->enqueue({.type = detail::task_type_e::reclaim, .stream = state_, .slot = std::move(current)});
        }
        service->enqueue({.type = detail::task_type_e::destroy_stream, .stream = state_, .slot = {}});
    }
}

std::optional<texture_upload_lease_s> texture_upload_stream_s::try_acquire_upload_buffer()
{
    std::shared_ptr<detail::texture_upload_slot_s> slot;
    bool                                           enqueue_allocation = false;
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->active) {
            return std::nullopt;
        }
        if (!state_->free_slots.empty()) {
            slot = std::move(state_->free_slots.front());
            state_->free_slots.pop_front();
            slot->state             = detail::slot_state_e::cpu_writing;
            slot->upload_id         = texture_upload_id_s{++state_->next_upload_id_value};
            slot->lease_released    = false;
            slot->discard_requested = false;
            ++state_->active_leases;
        } else if (state_->slots.size() + state_->pending_allocations < state_->config.max_slots &&
                   state_->pending_allocations == 0 &&
                   std::chrono::steady_clock::now() >= state_->retry_allocation_after) {
            ++state_->pending_allocations;
            enqueue_allocation = true;
        }
    }

    if (enqueue_allocation) {
        if (auto service = state_->service.lock()) {
            service->enqueue({.type = detail::task_type_e::allocate, .stream = state_, .slot = {}});
        }
    }
    if (!slot) {
        return std::nullopt;
    }
    return texture_upload_lease_s(state_, std::move(slot));
}

std::optional<texture_upload_lease_s>
texture_upload_stream_s::acquire_upload_buffer_for(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (auto lease = try_acquire_upload_buffer()) {
            return lease;
        }

        std::unique_lock lock(state_->mutex);
        if (!state_->active || state_->allocation_failed) {
            return std::nullopt;
        }
        if (!state_->slot_cv.wait_until(lock, deadline, [this] {
                return !state_->active || state_->allocation_failed || !state_->free_slots.empty();
            })) {
            return std::nullopt;
        }
    }
}

texture_frame_ptr texture_upload_stream_s::select_latest_completed_upload()
{
    return select_latest_completed_upload_through(texture_upload_id_s{std::numeric_limits<uint64_t>::max()});
}

texture_frame_ptr texture_upload_stream_s::select_latest_completed_upload_through(texture_upload_id_s upload_id)
{
    std::vector<std::shared_ptr<detail::texture_upload_slot_s>> reclaim;
    texture_frame_ptr                                           result;
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->active) {
            return nullptr;
        }

        std::shared_ptr<detail::texture_upload_slot_s> next;
        while (!state_->ready_slots.empty() && state_->ready_slots.front()->upload_id <= upload_id) {
            auto slot = std::move(state_->ready_slots.front());
            state_->ready_slots.pop_front();
            if (next) {
                next->state = detail::slot_state_e::reclaim;
                reclaim.emplace_back(std::move(next));
            }
            next = std::move(slot);
        }

        if (next) {
            if (auto current = retire_current_slot(state_)) {
                reclaim.emplace_back(std::move(current));
            }
            next->state                = detail::slot_state_e::current;
            state_->retained_upload_id = next->upload_id;
            state_->current_slot       = std::move(next);
        }

        if (state_->current_slot) {
            result = state_->current_slot->frame;
        }
    }

    if (auto service = state_->service.lock()) {
        for (auto& slot : reclaim) {
            service->enqueue({.type = detail::task_type_e::reclaim, .stream = state_, .slot = std::move(slot)});
        }
    }
    return result;
}

texture_frame_ptr texture_upload_stream_s::select_completed_upload(texture_upload_id_s upload_id)
{
    std::vector<std::shared_ptr<detail::texture_upload_slot_s>> reclaim;
    texture_frame_ptr                                           result;
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->active) {
            return nullptr;
        }

        const auto selected = std::ranges::find_if(
            state_->ready_slots, [upload_id](const auto& slot) { return slot->upload_id == upload_id; });
        if (selected == state_->ready_slots.end()) {
            return nullptr;
        }

        auto next = std::move(*selected);
        state_->ready_slots.erase(selected);
        for (auto& slot : state_->ready_slots) {
            slot->state = detail::slot_state_e::reclaim;
            reclaim.emplace_back(std::move(slot));
        }
        state_->ready_slots.clear();

        if (auto current = retire_current_slot(state_)) {
            reclaim.emplace_back(std::move(current));
        }
        next->state                = detail::slot_state_e::current;
        state_->retained_upload_id = next->upload_id;
        state_->current_slot       = std::move(next);
        result                     = state_->current_slot->frame;
    }

    if (auto service = state_->service.lock()) {
        for (auto& slot : reclaim) {
            service->enqueue({.type = detail::task_type_e::reclaim, .stream = state_, .slot = std::move(slot)});
        }
    }
    return result;
}

texture_frame_ptr texture_upload_stream_s::retained_frame_for(texture_upload_id_s upload_id) const
{
    const std::scoped_lock lock(state_->mutex);
    if (!state_->active || !state_->current_slot || state_->retained_upload_id != upload_id) {
        return nullptr;
    }
    return state_->current_slot->frame;
}

void texture_upload_stream_s::discard_upload(texture_upload_id_s upload_id)
{
    std::shared_ptr<detail::texture_upload_slot_s> reclaim;
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->active) {
            return;
        }

        const auto slot = std::ranges::find_if(
            state_->slots, [upload_id](const auto& candidate) { return candidate->upload_id == upload_id; });
        if (slot == state_->slots.end()) {
            return;
        }

        if ((*slot)->state == detail::slot_state_e::queued) {
            (*slot)->discard_requested = true;
            return;
        }
        if ((*slot)->state != detail::slot_state_e::ready) {
            return;
        }

        const auto ready = std::ranges::find(state_->ready_slots, *slot);
        if (ready == state_->ready_slots.end()) {
            return;
        }
        reclaim = std::move(*ready);
        state_->ready_slots.erase(ready);
        reclaim->discard_requested = true;
        reclaim->state             = detail::slot_state_e::reclaim;
    }

    if (auto service = state_->service.lock()) {
        service->enqueue({.type = detail::task_type_e::reclaim, .stream = state_, .slot = std::move(reclaim)});
    }
}

texture_upload_wait_result_e texture_upload_stream_s::wait_for_upload(texture_upload_id_s upload_id) const
{
    const auto find_slot = [this, upload_id] {
        return std::ranges::find_if(state_->slots,
                                    [upload_id](const auto& slot) { return slot->upload_id == upload_id; });
    };

    std::unique_lock lock(state_->mutex);
    state_->completion_cv.wait(lock, [this, &find_slot] {
        if (!state_->active) {
            return true;
        }
        const auto slot = find_slot();
        return slot == state_->slots.end() || (*slot)->state != detail::slot_state_e::queued;
    });
    if (!state_->active) {
        return texture_upload_wait_result_e::stopped;
    }
    const auto slot = find_slot();
    if (slot != state_->slots.end() &&
        ((*slot)->state == detail::slot_state_e::ready || (*slot)->state == detail::slot_state_e::current)) {
        return texture_upload_wait_result_e::ready;
    }
    return texture_upload_wait_result_e::failed;
}

texture_upload_id_s texture_upload_stream_s::latest_completed_upload_id() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->ready_slots.empty() ? texture_upload_id_s{} : state_->ready_slots.back()->upload_id;
}

texture_upload_id_s texture_upload_stream_s::retained_upload_id() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->retained_upload_id;
}

bool texture_upload_stream_s::allocation_failed() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->allocation_failed;
}

texture_upload_config_s texture_upload_stream_s::configuration() const noexcept { return state_->config; }

texture_upload_service_s::texture_upload_service_s(context_s* parent, size_t memory_budget)
    : state_(std::make_shared<detail::texture_upload_service_state_s>(parent, memory_budget))
{
    state_->start();
}

texture_upload_service_s::~texture_upload_service_s()
{
    state_->stop();
    state_.reset();
}

std::shared_ptr<texture_upload_stream_s> texture_upload_service_s::create_stream(texture_upload_config_s config)
{
    if (config.max_slots == 0 || config.initial_slots > config.max_slots ||
        config.transfer_layout.host_memory_access == host_memory_access_e::read_only) {
        throw std::invalid_argument("invalid texture upload stream configuration");
    }
    detail::normalize_transfer_layout(config.transfer_layout);
    auto stream                 = std::make_shared<detail::texture_upload_stream_state_s>();
    stream->service             = state_;
    stream->config              = config;
    stream->pending_allocations = config.initial_slots;
    auto result                 = std::shared_ptr<texture_upload_stream_s>(new texture_upload_stream_s(stream));
    for (size_t index = 0; index < config.initial_slots; ++index) {
        state_->enqueue({.type = detail::task_type_e::allocate, .stream = stream, .slot = {}});
    }
    return result;
}

size_t texture_upload_service_s::memory_usage() const noexcept { return state_->memory_usage(); }

size_t texture_upload_service_s::memory_budget() const noexcept { return state_->memory_budget(); }

} // namespace miximus::gpu::transfer
