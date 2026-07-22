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
constexpr auto upload_timeout         = std::chrono::milliseconds(20);
} // namespace

ULONG input_video_buffer_s::Release()
{
    const ULONG count = --ref_count_;
    if (count == 0) {
        allocator_->return_buffer(this);
    }
    return count;
}

HRESULT input_video_buffer_s::StartAccess(BMDBufferAccessFlags flags)
{
    if ((flags & bmdBufferAccessWrite) == 0) {
        return S_OK;
    }

    // DeckLink allocates a small set of buffer objects and cycles them. A
    // fresh one-shot transfer lease is therefore needed for every write
    // access, not only when the IDeckLinkVideoBuffer object is allocated.
    upload_.reset();
    auto upload = allocator_->acquire_upload(first_access_);
    if (!upload) {
        return E_OUTOFMEMORY;
    }
    first_access_ = false;
    upload_.emplace(std::move(*upload));
    return S_OK;
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

    // Initial backend allocation may queue behind application startup. Once
    // warm, waiting at most one frame bounds capture-thread backpressure while
    // leaving the render thread entirely uninvolved.
    return stream->acquire_for(first_access ? initial_upload_timeout : upload_timeout);
}

HRESULT input_video_buffer_allocator_s::AllocateVideoBuffer(IDeckLinkVideoBuffer** allocatedBuffer)
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
        getlog("decklink")->error("DeckLink buffer allocation failed: {}", error.what());
    } catch (...) {
        getlog("decklink")->error("DeckLink buffer allocation failed");
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
