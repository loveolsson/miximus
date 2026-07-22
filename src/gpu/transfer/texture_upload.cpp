#include "texture_upload.hpp"

#include "gpu/context.hpp"
#include "gpu/sync.hpp"
#include "gpu/transfer/detail/backend_factory.hpp"
#include "gpu/transfer/detail/requirements.hpp"
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
    std::unique_ptr<backend_i> backend;
    std::unique_ptr<texture_s> texture;
    slot_state_e               state{slot_state_e::free};
    size_t                     reserved_bytes{};
    bool                       gl_owns_texture{};
    bool                       lease_released{true};
    uint64_t                   version{};
};

struct texture_upload_stream_state_s
{
    std::weak_ptr<texture_upload_service_state_s>       service;
    texture_upload_desc_s                               desc;
    mutable std::mutex                                  mutex;
    std::condition_variable                             slot_cv;
    std::condition_variable                             completion_cv;
    std::vector<std::shared_ptr<texture_upload_slot_s>> slots;
    std::deque<std::shared_ptr<texture_upload_slot_s>>  free_slots;
    std::deque<std::shared_ptr<texture_upload_slot_s>>  ready_slots;
    std::shared_ptr<texture_upload_slot_s>              current_slot;
    size_t                                              pending_allocations{};
    size_t                                              active_leases{};
    uint64_t                                            next_version{};
    uint64_t                                            completed_version{};
    uint64_t                                            current_version{};
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
        if (slot.texture && slot.backend) {
            if (slot.gl_owns_texture) {
                slot.backend->end_texture_use();
                slot.gl_owns_texture = false;
            }
            slot.backend->unbind_texture();
        }
        slot.backend.reset();
        slot.texture.reset();
        release_memory(slot.reserved_bytes);
        slot.reserved_bytes = 0;
    }

    void allocate_slot(const std::shared_ptr<texture_upload_stream_state_s>& stream)
    {
        size_t reserved_bytes  = 0;
        bool   memory_reserved = false;
        try {
            reserved_bytes = estimate_slot_memory_usage(stream->desc.requirements);
            if (!reserve_memory(reserved_bytes)) {
                throw std::bad_alloc();
            }
            memory_reserved = true;

            auto slot            = std::make_shared<texture_upload_slot_s>();
            slot->reserved_bytes = reserved_bytes;
            slot->texture =
                std::make_unique<texture_s>(stream->desc.requirements.dimensions, stream->desc.requirements.format);
            auto backend =
                create_backend(stream->desc.requirements, backend_i::direction_e::cpu_to_gpu, slot->texture.get());
            slot->backend = std::move(backend.backend);

            const auto actual_reserved = slot_memory_usage(stream->desc.requirements, backend.allocation_bytes);
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
        if (slot->gl_owns_texture) {
            success               = slot->backend->end_texture_use();
            slot->gl_owns_texture = false;
        }
        success               = slot->backend->transfer() && success;
        success               = slot->backend->wait_for_completion() && success;
        slot->gl_owns_texture = success;

        if (success && stream->desc.generate_mip_maps) {
            slot->texture->generate_mip_maps();
        }

        if (success) {
            sync_s ready_sync;
            context_s::flush();
            // The render thread must never inherit an unsignalled upload wait.
            // Waiting here keeps transfer latency entirely on this dedicated
            // worker and publishes only textures that are immediately usable.
            success = ready_sync.cpu_wait(std::chrono::hours(1));
        }

        const std::scoped_lock lock(stream->mutex);
        stream->completed_version = std::max(stream->completed_version, slot->version);
        if (!success || !stream->active) {
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

std::span<std::byte> texture_upload_lease_s::bytes() const
{
    if (!slot_) {
        return {};
    }
    return {static_cast<std::byte*>(slot_->backend->data()), slot_->backend->size()};
}

uint64_t texture_upload_lease_s::version() const { return slot_ ? slot_->version : 0; }

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
    auto service = state_->service.lock();
    {
        const std::scoped_lock lock(state_->mutex);
        state_->active = false;
        state_->completion_cv.notify_all();
    }
    if (service) {
        service->enqueue({.type = detail::task_type_e::destroy_stream, .stream = state_, .slot = {}});
    }
}

std::optional<texture_upload_lease_s> texture_upload_stream_s::try_acquire()
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
            slot->state          = detail::slot_state_e::cpu_writing;
            slot->version        = ++state_->next_version;
            slot->lease_released = false;
            ++state_->active_leases;
        } else if (state_->slots.size() + state_->pending_allocations < state_->desc.max_slots &&
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

std::optional<texture_upload_lease_s> texture_upload_stream_s::acquire_for(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (auto lease = try_acquire()) {
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

texture_s* texture_upload_stream_s::consume_latest() { return consume_through(std::numeric_limits<uint64_t>::max()); }

texture_s* texture_upload_stream_s::consume_through(uint64_t version)
{
    std::vector<std::shared_ptr<detail::texture_upload_slot_s>> reclaim;
    texture_s*                                                  result = nullptr;
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->active) {
            return nullptr;
        }

        std::shared_ptr<detail::texture_upload_slot_s> next;
        while (!state_->ready_slots.empty() && state_->ready_slots.front()->version <= version) {
            auto slot = std::move(state_->ready_slots.front());
            state_->ready_slots.pop_front();
            if (next) {
                next->state = detail::slot_state_e::reclaim;
                reclaim.emplace_back(std::move(next));
            }
            next = std::move(slot);
        }

        if (next) {
            if (state_->current_slot) {
                state_->current_slot->state = detail::slot_state_e::reclaim;
                reclaim.emplace_back(std::move(state_->current_slot));
            }
            next->state             = detail::slot_state_e::current;
            state_->current_version = next->version;
            state_->current_slot    = std::move(next);
        }

        if (state_->current_slot) {
            result = state_->current_slot->texture.get();
        }
    }

    if (auto service = state_->service.lock()) {
        for (auto& slot : reclaim) {
            service->enqueue({.type = detail::task_type_e::reclaim, .stream = state_, .slot = std::move(slot)});
        }
    }
    return result;
}

texture_upload_wait_result_e texture_upload_stream_s::wait_until_ready(uint64_t version) const
{
    const auto is_ready = [this, version] {
        if (state_->current_slot && state_->current_slot->version == version) {
            return true;
        }
        return std::ranges::any_of(state_->ready_slots,
                                   [version](const auto& slot) { return slot->version == version; });
    };

    std::unique_lock lock(state_->mutex);
    state_->completion_cv.wait(lock, [this, version, &is_ready] {
        return !state_->active || is_ready() || state_->completed_version >= version;
    });
    if (is_ready()) {
        return texture_upload_wait_result_e::ready;
    }
    return state_->active ? texture_upload_wait_result_e::failed : texture_upload_wait_result_e::stopped;
}

uint64_t texture_upload_stream_s::latest_ready_version() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->ready_slots.empty() ? 0 : state_->ready_slots.back()->version;
}

uint64_t texture_upload_stream_s::current_version() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->current_version;
}

bool texture_upload_stream_s::allocation_failed() const
{
    const std::scoped_lock lock(state_->mutex);
    return state_->allocation_failed;
}

texture_upload_desc_s texture_upload_stream_s::desc() const { return state_->desc; }

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

std::shared_ptr<texture_upload_stream_s> texture_upload_service_s::create_stream(texture_upload_desc_s desc)
{
    if (desc.max_slots == 0 || desc.requirements.host_access == host_access_e::read_only) {
        throw std::invalid_argument("invalid texture upload stream description");
    }
    detail::normalize_requirements(desc.requirements);
    auto stream     = std::make_shared<detail::texture_upload_stream_state_s>();
    stream->service = state_;
    stream->desc    = desc;
    return std::shared_ptr<texture_upload_stream_s>(new texture_upload_stream_s(std::move(stream)));
}

size_t texture_upload_service_s::memory_usage() const { return state_->memory_usage(); }

size_t texture_upload_service_s::memory_budget() const { return state_->memory_budget(); }

} // namespace miximus::gpu::transfer
