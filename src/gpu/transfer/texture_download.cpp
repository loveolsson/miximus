#include "texture_download.hpp"

#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/sync.hpp"
#include "gpu/transfer/transfer.hpp"
#include "logger/logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
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

size_t checked_add(size_t lhs, size_t rhs)
{
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        throw std::overflow_error("texture download allocation size overflow");
    }
    return lhs + rhs;
}

size_t checked_multiply(size_t lhs, size_t rhs)
{
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        throw std::overflow_error("texture download allocation size overflow");
    }
    return lhs * rhs;
}

size_t estimate_slot_bytes(const texture_download_desc_s& desc)
{
    // Conservatively account for the CPU/PBO transfer allocation and the
    // render-target texture. Packed YUV formats occupy four bytes per texel.
    size_t bytes_per_pixel = 4;
    if (desc.format == texture_s::format_e::rgb_f16) {
        bytes_per_pixel = 6;
    } else if (desc.format == texture_s::format_e::rgba_f16) {
        bytes_per_pixel = 8;
    }
    const auto texture_bytes = checked_multiply(
        checked_multiply(static_cast<size_t>(desc.dimensions.x), static_cast<size_t>(desc.dimensions.y)),
        bytes_per_pixel);
    // CUDA readback owns pinned host storage and an interop PBO. Other
    // backends use no more, so this is a conservative backend-independent cap.
    return checked_add(checked_multiply(desc.byte_size, 2), texture_bytes);
}
} // namespace

struct texture_download_slot_s
{
    std::unique_ptr<transfer_i> transfer;
    std::unique_ptr<texture_s>  texture;
    std::unique_ptr<sync_s>     render_sync;
    slot_state_e                state{slot_state_e::free};
    size_t                      reserved_bytes{};
    bool                        registered{};
    uint64_t                    tag{};
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

struct texture_download_service_state_s : std::enable_shared_from_this<texture_download_service_state_s>
{
    std::mutex              queue_mutex;
    std::condition_variable queue_cv;
    std::deque<task_s>      tasks;
    bool                    stopping{};

    std::unique_ptr<context_s> context;
    std::thread                worker;
    const size_t               memory_budget;
    std::atomic_size_t         memory_usage;

    texture_download_service_state_s(context_s* parent, size_t budget)
        : context(context_s::create_unique_context(false, parent))
        , memory_budget(budget)
    {
    }

    void start()
    {
        auto self = shared_from_this();
        worker    = std::thread([self = std::move(self)] { self->run(); });
    }

    void enqueue(task_s task)
    {
        {
            const std::scoped_lock lock(queue_mutex);
            if (stopping) {
                return;
            }
            tasks.emplace_back(std::move(task));
        }
        queue_cv.notify_one();
    }

    bool reserve_memory(size_t bytes)
    {
        auto current = memory_usage.load(std::memory_order_relaxed);
        while (bytes <= memory_budget && current <= memory_budget - bytes) {
            if (memory_usage.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void release_slot(texture_download_slot_s& slot)
    {
        if (slot.render_sync) {
            slot.render_sync->gpu_wait();
            slot.render_sync.reset();
        }
        if (slot.texture && slot.registered) {
            (void)transfer_i::begin_texture_use(slot.transfer->type(), slot.texture.get());
            (void)transfer_i::unregister_texture(slot.transfer->type(), slot.texture.get());
        }
        slot.registered = false;
        slot.transfer.reset();
        slot.texture.reset();
        memory_usage.fetch_sub(slot.reserved_bytes, std::memory_order_relaxed);
        slot.reserved_bytes = 0;
    }

    void allocate_slot(const std::shared_ptr<texture_download_stream_state_s>& stream)
    {
        size_t reserved{};
        bool   reserved_memory{};
        try {
            reserved = estimate_slot_bytes(stream->desc);
            if (!reserve_memory(reserved)) {
                throw std::bad_alloc();
            }
            reserved_memory = true;

            auto slot            = std::make_shared<texture_download_slot_s>();
            slot->reserved_bytes = reserved;
            slot->texture        = std::make_unique<texture_s>(stream->desc.dimensions, stream->desc.format);
            slot->transfer       = transfer_i::create_transfer(
                transfer_i::get_prefered_type(), stream->desc.byte_size, transfer_i::direction_e::gpu_to_cpu);
            slot->registered = transfer_i::register_texture(slot->transfer->type(), slot->texture.get());
            if (!slot->registered) {
                throw std::runtime_error("failed to register download texture with transfer backend");
            }
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
                memory_usage.fetch_sub(reserved, std::memory_order_relaxed);
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
        success = slot->transfer->perform_transfer(slot->texture.get()) && success;
        success = slot->transfer->wait_for_copy() && success;

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

    void run()
    {
        const context_scope_s context_scope(*context);
        std::deque<task_s>    delayed;
        while (true) {
            task_s task{};
            {
                std::unique_lock lock(queue_mutex);
                if (tasks.empty() && !stopping) {
                    if (delayed.empty()) {
                        queue_cv.wait(lock, [this] { return stopping || !tasks.empty(); });
                    } else {
                        queue_cv.wait_for(
                            lock, std::chrono::milliseconds(1), [this] { return stopping || !tasks.empty(); });
                    }
                }
                if (stopping && tasks.empty() && delayed.empty()) {
                    break;
                }
                if (!tasks.empty()) {
                    task = std::move(tasks.front());
                    tasks.pop_front();
                } else if (!delayed.empty()) {
                    task = std::move(delayed.front());
                    delayed.pop_front();
                } else {
                    continue;
                }
            }

            switch (task.type) {
                case task_type_e::allocate:
                    allocate_slot(task.stream);
                    break;
                case task_type_e::download:
                    download_slot(task.stream, task.slot);
                    break;
                case task_type_e::destroy_stream:
                    if (!destroy_stream(task.stream)) {
                        delayed.emplace_back(std::move(task));
                    }
                    break;
            }
        }
    }

    void stop()
    {
        {
            const std::scoped_lock lock(queue_mutex);
            stopping = true;
        }
        queue_cv.notify_one();
        if (worker.joinable()) {
            worker.join();
        }
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

void* texture_download_frame_s::ptr() const { return slot_ ? slot_->transfer->ptr() : nullptr; }

size_t texture_download_frame_s::size() const { return slot_ ? slot_->transfer->size() : 0; }

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
    if (!transfer_i::begin_texture_use(slot->transfer->type(), slot->texture.get())) {
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
    if (desc.dimensions.x <= 0 || desc.dimensions.y <= 0 || desc.byte_size == 0 || desc.max_slots == 0) {
        throw std::invalid_argument("invalid texture download stream description");
    }
    auto stream     = std::make_shared<detail::texture_download_stream_state_s>();
    stream->service = state_;
    stream->desc    = desc;
    return std::shared_ptr<texture_download_stream_s>(new texture_download_stream_s(std::move(stream)));
}

size_t texture_download_service_s::memory_usage() const { return state_->memory_usage.load(std::memory_order_relaxed); }

size_t texture_download_service_s::memory_budget() const { return state_->memory_budget; }

} // namespace miximus::gpu::transfer
