#include "allocator.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <exception>
#include <memory>
#include <utility>

namespace miximus::nodes::decklink::detail {
using namespace miximus::decklink_sdk;

namespace {
constexpr auto initial_upload_timeout = std::chrono::seconds(2);
} // namespace

HRESULT input_video_buffer_s::StartAccess(BMDBufferAccessFlags flags) noexcept
{
    try {
        if ((flags & bmdBufferAccessWrite) == 0) {
            return S_OK;
        }

        upload_.reset();
        upload_ = allocator_->acquire_upload(first_access_);
        if (upload_.has_value()) {
            first_access_ = false;
        }
        return upload_.has_value() ? S_OK : E_OUTOFMEMORY;
    } catch (...) {
        logger::log_error_noexcept("decklink", "DeckLink buffer access failed");
        return E_OUTOFMEMORY;
    }
}

ULONG input_video_buffer_s::Release() noexcept
{
    const ULONG count = --ref_count_;
    if (count == 0) {
        try {
            allocator_->return_buffer(this);
        } catch (...) {
            logger::log_error_noexcept("decklink", "DeckLink buffer release failed");
        }
    }
    return count;
}

input_video_buffer_allocator_s::~input_video_buffer_allocator_s() { assert(active_buffers_ == 0); }

auto input_video_buffer_allocator_s::acquire_upload(bool first_access)
    -> std::optional<gpu::transfer::texture_upload_lease_s>
{
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
    {
        const std::scoped_lock lock(mutex_);
        stream = shutting_down_ ? nullptr : upload_stream_;
    }
    if (!stream) {
        return std::nullopt;
    }

    const auto start = std::chrono::steady_clock::now();
    auto       upload =
        first_access ? stream->acquire_upload_buffer_for(initial_upload_timeout) : stream->try_acquire_upload_buffer();
    const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    const auto wait_us = static_cast<uint64_t>(wait.count());
    auto       maximum = upload_acquire_wait_max_us_.load();
    while (wait_us > maximum && !upload_acquire_wait_max_us_.compare_exchange_weak(maximum, wait_us)) {
    }
    if (wait >= std::chrono::milliseconds(1)) {
        ++upload_acquire_slow_count_;
    }
    if (!upload.has_value()) {
        ++upload_acquire_failures_;
    }
    return upload;
}

HRESULT input_video_buffer_allocator_s::AllocateVideoBuffer(IDeckLinkVideoBuffer** allocatedBuffer) noexcept
{
    if (allocatedBuffer == nullptr) {
        return E_POINTER;
    }
    *allocatedBuffer = nullptr;

    try {
        const std::scoped_lock lock(mutex_);
        if (shutting_down_ || !upload_stream_) {
            return E_OUTOFMEMORY;
        }
        const auto slot =
            std::ranges::find_if(buffers_, [](const buffer_slot_s& candidate) { return !candidate.active; });
        if (slot == buffers_.end()) {
            return E_OUTOFMEMORY;
        }

        if (!slot->buffer) {
            slot->buffer = std::make_unique<input_video_buffer_s>(this, buffer_size_);
        }
        slot->active = true;
        ++active_buffers_;
        slot->buffer->activate();

        *allocatedBuffer = static_cast<IDeckLinkVideoBuffer*>(slot->buffer.get());
        return S_OK;
    } catch (const std::exception& error) {
        logger::log_error_noexcept("decklink", "DeckLink buffer allocation failed: {}", error.what());
    } catch (...) {
        logger::log_error_noexcept("decklink", "DeckLink buffer allocation failed");
    }
    return E_OUTOFMEMORY;
}

void input_video_buffer_allocator_s::return_buffer(input_video_buffer_s* buffer)
{
    const std::scoped_lock lock(mutex_);
    const auto             slot = std::ranges::find_if(
        buffers_, [buffer](const buffer_slot_s& candidate) { return candidate.buffer.get() == buffer; });
    if (slot == buffers_.end() || !slot->active) {
        getlog("decklink")->error("DeckLink allocator tried to release an unknown buffer");
        return;
    }

    buffer->clear_upload();
    slot->active = false;
    --active_buffers_;
    if (shutting_down_ && active_buffers_ == 0) {
        idle_condition_.notify_all();
    }
}

decklink_ptr<input_video_buffer_s> query_input_video_buffer(IUnknown* source)
{
    if (source == nullptr) {
        return {};
    }

    input_video_buffer_s* buffer = nullptr;
    if (FAILED(source->QueryInterface(input_video_buffer_iid(), reinterpret_cast<void**>(&buffer)))) {
        return {};
    }
    return decklink_ptr<input_video_buffer_s>(buffer, false);
}

void input_video_buffer_allocator_s::shutdown_and_wait()
{
    std::unique_lock lock(mutex_);
    shutting_down_ = true;
    upload_stream_.reset();
    idle_condition_.wait(lock, [this] { return active_buffers_ == 0; });
}

} // namespace miximus::nodes::decklink::detail
