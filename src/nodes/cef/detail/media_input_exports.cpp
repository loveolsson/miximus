#include "media_input_exports.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace miximus::nodes::cef::detail {
namespace {
constexpr size_t INPUTS = media_input_pool_s::INPUT_COUNT;
constexpr size_t SLOTS  = media_input_pool_s::MAX_DEPTH;
} // namespace

struct media_input_exports_s::quarantine_s::impl_s
{
    std::mutex                            mutex;
    std::vector<std::shared_ptr<state_s>> retained;
};
media_input_exports_s::quarantine_s::quarantine_s()
    : impl_(std::make_unique<impl_s>())
{
}
media_input_exports_s::quarantine_s::~quarantine_s() = default;

struct media_input_exports_s::pending_s
{
    media_input_pool_s::ticket_s                   ticket;
    std::shared_ptr<gpu::detail::dma_buf_export_s> image;
    gpu::completion_s                              completion;
    int64_t                                        timestamp{};
    bool abandoned{}, submitted{}, producer_done{}, committed{}, cancelled{}, consuming{};
};

struct media_input_exports_s::state_s
{
    struct input_s
    {
        uint64_t                                                          revision{};
        gpu::extent_s                                                     extent{};
        bool                                                              active{}, configuring{};
        std::array<std::shared_ptr<gpu::detail::dma_buf_export_s>, SLOTS> images;
        std::array<std::shared_ptr<pending_s>, SLOTS>                     pending;
    };
    gpu::device_s&              device;
    const size_t                depth;
    quarantine_s&               quarantine;
    std::mutex                  configure_mutex;
    const size_t                byte_budget;
    std::shared_ptr<void>       budget_lease;
    mutable std::mutex          mutex;
    media_input_pool_s          pool;
    std::array<input_s, INPUTS> inputs;
    bool                        failed{};
    size_t                      next_input{};

    state_s(gpu::device_s& gpu, size_t count, quarantine_s& owner, size_t budget, std::shared_ptr<void> lease)
        : device(gpu)
        , depth(count)
        , quarantine(owner)
        , byte_budget(budget)
        , budget_lease(std::move(lease))
        , pool(count)
    {
    }

    static void validate(size_t input)
    {
        if (input >= INPUTS)
            throw std::out_of_range("Invalid browser input index");
    }
    void revoke(size_t input)
    {
        inputs[input].active = false;
        ++inputs[input].revision;
        pool.invalidate(input);
        for (auto& pending : inputs[input].pending)
            if (pending)
                pending->cancelled = true;
    }
    static void quarantine_state(const std::shared_ptr<state_s>& state)
    {
        std::lock_guard lock(state->mutex);
        if (state->failed)
            return;
        // Retain before setting failure, so allocation failure cannot falsely
        // claim that unproven foreign reads are covered by quarantine.
        std::lock_guard quarantine_lock(state->quarantine.impl_->mutex);
        state->quarantine.impl_->retained.push_back(state);
        state->failed = true;
        for (size_t input = 0; input < INPUTS; ++input)
            state->revoke(input);
    }
};

media_input_exports_s::publication_s::publication_s(std::shared_ptr<state_s> state, std::shared_ptr<pending_s> pending)
    : state_(std::move(state))
    , pending_(std::move(pending))
{
}
media_input_exports_s::publication_s::~publication_s()
{
    if (!committed_) {
        std::lock_guard lock(state_->mutex);
        pending_->cancelled = true;
        state_->pool.cancel(pending_->ticket);
    }
}
void media_input_exports_s::publication_s::commit()
{
    std::lock_guard lock(state_->mutex);
    committed_          = true;
    pending_->committed = true;
}

media_input_exports_s::frame_s::frame_s(std::shared_ptr<state_s> state, std::shared_ptr<pending_s> pending)
    : state_(std::move(state))
    , pending_(std::move(pending))
{
}
media_input_exports_s::frame_s::~frame_s() { retire(false); }
media_input_pool_s::ticket_s         media_input_exports_s::frame_s::ticket() const { return pending_->ticket; }
const gpu::detail::dma_buf_export_s& media_input_exports_s::frame_s::image() const
{
    if (retired_)
        throw std::logic_error("Browser input frame has already retired");
    return *pending_->image;
}
int64_t media_input_exports_s::frame_s::timestamp_us() const { return pending_->timestamp; }
bool    media_input_exports_s::frame_s::current() const
{
    std::lock_guard lock(state_->mutex);
    return !retired_ && !pending_->cancelled && !state_->failed;
}
uint64_t media_input_exports_s::generation(size_t input) const
{
    state_s::validate(input);
    std::lock_guard lock(state_->mutex);
    return state_->inputs[input].revision + 1;
}
void media_input_exports_s::frame_s::retire(bool safe)
{
    {
        std::lock_guard lock(state_->mutex);
        if (retired_)
            return;
        retired_ = true;
        if (safe) {
            state_->pool.consumer_finished(pending_->ticket);
            auto& entry = state_->inputs[pending_->ticket.input].pending[pending_->ticket.slot];
            if (entry == pending_)
                entry.reset();
        }
    }
    if (!safe)
        state_s::quarantine_state(state_);
}

media_input_exports_s::media_input_exports_s(gpu::device_s&        device,
                                             size_t                depth,
                                             quarantine_s&         quarantine,
                                             size_t                byte_budget,
                                             std::shared_ptr<void> budget_lease)
    : state_(std::make_shared<state_s>(device, depth, quarantine, byte_budget, std::move(budget_lease)))
{
}
media_input_exports_s::~media_input_exports_s() = default;

bool media_input_exports_s::configure(size_t input, gpu::extent_s extent)
{
    state_s::validate(input);
    if (!extent.width || !extent.height || extent.width > 4096 || extent.height > 4096)
        throw std::invalid_argument("Browser input extent must be in [1, 4096]");
    auto&                                                             state = *state_;
    std::lock_guard                                                   configuration_lock(state.configure_mutex);
    uint64_t                                                          revision{};
    size_t                                                            available{};
    std::array<std::shared_ptr<gpu::detail::dma_buf_export_s>, SLOTS> images;
    {
        std::lock_guard lock(state.mutex);
        auto&           entry = state.inputs[input];
        if (state.failed || entry.configuring)
            return false;
        if (entry.active && entry.extent == extent)
            return true;
        if (entry.active)
            state.revoke(input);
        if (state.pool.metrics(input).occupied)
            return false;
        for (size_t slot = 0; slot < state.depth; ++slot)
            if (entry.pending[slot] || (entry.images[slot] && entry.images[slot].use_count() != 1))
                return false;
        if (entry.extent == extent && entry.images[0]) {
            state.revoke(input);
            entry.active = true;
            return true;
        }
        size_t other_bytes{};
        for (size_t other = 0; other < INPUTS; ++other)
            if (other != input)
                for (const auto& image : state.inputs[other].images)
                    if (image)
                        other_bytes += image->allocation_bytes();
        available = state.byte_budget - other_bytes;
        if (uint64_t(extent.width) * extent.height * 4 * state.depth > available)
            throw std::runtime_error("Browser input export byte budget exhausted");
        state.revoke(input);
        revision          = entry.revision;
        entry.configuring = true;
        images.swap(entry.images);
    }
    // Destroy old allocations before allocating replacements, outside render locks.
    images = {};
    try {
        size_t allocated{};
        for (size_t slot = 0; slot < state.depth; ++slot) {
            images[slot] = std::make_shared<gpu::detail::dma_buf_export_s>(state.device, extent);
            allocated += images[slot]->allocation_bytes();
            if (allocated > available)
                throw std::runtime_error("Browser input export allocation exceeds byte budget");
        }
    } catch (...) {
        std::lock_guard lock(state.mutex);
        state.inputs[input].configuring = false;
        throw;
    }
    std::lock_guard lock(state.mutex);
    auto&           entry = state.inputs[input];
    entry.images.swap(images);
    entry.extent      = extent;
    entry.configuring = false;
    entry.active      = !state.failed && entry.revision == revision;
    return entry.active;
}

void media_input_exports_s::invalidate(size_t input)
{
    state_s::validate(input);
    std::lock_guard lock(state_->mutex);
    state_->revoke(input);
}

std::shared_ptr<media_input_exports_s::publication_s> media_input_exports_s::record(size_t                input,
                                                                                    gpu::recording_s&     recording,
                                                                                    const gpu::texture_s& source,
                                                                                    int64_t               timestamp_us)
{
    state_s::validate(input);
    auto state   = state_;
    auto pending = std::make_shared<pending_s>();
    // Separate native-submission and complete-frame decisions. An early native
    // flush followed by a graph exception must still drain producer GPU work.
    struct native_guard_s
    {
        std::shared_ptr<state_s>   state;
        std::shared_ptr<pending_s> pending;
        ~native_guard_s()
        {
            std::lock_guard lock(state->mutex);
            if (!pending->submitted) {
                state->pool.abandon(pending->ticket);
                pending->abandoned = true;
            }
        }
    };
    auto native      = std::make_shared<native_guard_s>();
    native->state    = state;
    native->pending  = pending;
    auto publication = std::shared_ptr<publication_s>(new publication_s(state, pending));
    {
        std::lock_guard lock(state->mutex);
        auto&           entry = state->inputs[input];
        if (state->failed || !entry.active || entry.configuring)
            return nullptr;
        const auto ticket = state->pool.acquire(input);
        if (!ticket)
            return nullptr;
        pending->ticket             = *ticket;
        pending->image              = entry.images[ticket->slot];
        pending->timestamp          = timestamp_us;
        entry.pending[ticket->slot] = pending;
    }
    gpu::draw_s conversion;
    conversion.compositing = gpu::compositing_e::replace;
    conversion.transfer    = gpu::color_operation_e::encode_srgb_premultiplied;
    pending->image->copy(recording, source, conversion);
    recording.on_submitted([native](gpu::completion_s completion) {
        std::lock_guard lock(native->state->mutex);
        native->pending->completion = std::move(completion);
        native->pending->submitted  = true;
        native->state->pool.publish(native->pending->ticket);
    });
    return publication;
}

std::shared_ptr<media_input_exports_s::frame_s> media_input_exports_s::poll()
{
    auto                                                   state = state_;
    std::array<std::shared_ptr<pending_s>, INPUTS * SLOTS> candidates;
    {
        std::lock_guard lock(state->mutex);
        if (state->failed)
            return nullptr;
        for (size_t offset = 0; offset < INPUTS; ++offset) {
            const size_t input = (state->next_input + offset) % INPUTS;
            auto         first = candidates.begin() + static_cast<std::ptrdiff_t>(offset * SLOTS);
            for (size_t slot = 0; slot < state->depth; ++slot)
                candidates[offset * SLOTS + slot] = state->inputs[input].pending[slot];
            std::sort(first, first + static_cast<std::ptrdiff_t>(state->depth), [](const auto& a, const auto& b) {
                if (!b)
                    return bool(a);
                return a && a->ticket.serial < b->ticket.serial;
            });
        }
    }
    std::array<bool, INPUTS> blocked{};
    for (const auto& pending : candidates) {
        if (!pending || blocked[pending->ticket.input])
            continue;
        gpu::completion_s completion;
        {
            std::lock_guard lock(state->mutex);
            if (pending->consuming)
                continue;
            completion = pending->completion;
        }
        // Driver queries run outside the metadata lock and off the render thread.
        const auto ready = completion ? completion.wait(std::chrono::milliseconds(0)) : gpu::wait_result_e::timeout;
        if (completion && ready != gpu::wait_result_e::ready && ready != gpu::wait_result_e::timeout) {
            state_s::quarantine_state(state);
            return nullptr;
        }
        std::lock_guard lock(state->mutex);
        auto&           entry = state->inputs[pending->ticket.input].pending[pending->ticket.slot];
        if (entry != pending || state->failed)
            continue;
        if (pending->abandoned) {
            entry.reset();
            continue;
        }
        if (!pending->producer_done && ready == gpu::wait_result_e::ready) {
            state->pool.producer_finished(pending->ticket);
            pending->producer_done = true;
        }
        if (!pending->producer_done) {
            blocked[pending->ticket.input] = true;
            continue;
        }
        if (pending->cancelled) {
            entry.reset();
            continue;
        }
        if (pending->committed) {
            auto frame = std::shared_ptr<frame_s>(new frame_s(state, pending));
            if (state->pool.begin_consume(pending->ticket)) {
                state->next_input  = (pending->ticket.input + 1) % INPUTS;
                pending->consuming = true;
                frame->retired_    = false;
                return frame;
            }
        } else {
            blocked[pending->ticket.input] = true;
        }
    }
    return nullptr;
}

bool media_input_exports_s::idle() const
{
    std::lock_guard lock(state_->mutex);
    for (size_t input = 0; input < INPUTS; ++input)
        if (state_->inputs[input].configuring || state_->pool.metrics(input).occupied)
            return false;
    return true;
}
size_t media_input_exports_s::allocated_bytes() const
{
    std::lock_guard lock(state_->mutex);
    size_t          bytes{};
    for (const auto& input : state_->inputs)
        for (const auto& image : input.images)
            if (image)
                bytes += image->allocation_bytes();
    return bytes;
}
bool media_input_exports_s::failed() const
{
    std::lock_guard lock(state_->mutex);
    return state_->failed;
}
media_input_pool_s::metrics_s media_input_exports_s::metrics(size_t input) const { return state_->pool.metrics(input); }
} // namespace miximus::nodes::cef::detail
