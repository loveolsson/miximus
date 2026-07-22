#include "allocator.hpp"

#include "logger/logger.hpp"

#include <cassert>
#include <chrono>
#include <exception>
#include <memory>
#include <utility>

namespace miximus::nodes::decklink::detail {
namespace {
constexpr size_t max_allocations        = 6;
constexpr auto   initial_upload_timeout = std::chrono::seconds(2);
constexpr auto   upload_timeout         = std::chrono::milliseconds(20);
} // namespace

ULONG video_buffer_s::Release()
{
    const ULONG count = --ref_count_;
    if (count == 0) {
        allocator_->return_buffer(this);
    }
    return count;
}

HRESULT video_buffer_s::StartAccess(BMDBufferAccessFlags flags)
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

allocator_s::~allocator_s()
{
    assert(free_buffers_.empty());
    assert(allocated_buffers_.empty());
}

void allocator_s::set_upload_stream(std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream)
{
    const std::scoped_lock lock(mutex_);
    if (shutting_down_) {
        return;
    }
    upload_stream_ = std::move(stream);
}

auto allocator_s::acquire_upload(bool first_access) -> std::optional<gpu::transfer::texture_upload_lease_s>
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

HRESULT allocator_s::AllocateVideoBuffer(IDeckLinkVideoBuffer** allocatedBuffer)
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
        if (!free_buffers_.empty()) {
            auto reusable = std::move(free_buffers_.front());
            free_buffers_.pop_front();
            auto* raw = reusable.get();
            raw->reset_ref_count();
            allocated_buffers_.emplace(raw, std::move(reusable));
            *allocatedBuffer = static_cast<IDeckLinkVideoBuffer*>(raw);
            return S_OK;
        }

        if (allocated_buffers_.size() >= max_allocations) {
            return E_OUTOFMEMORY;
        }

        auto  buffer = std::make_unique<video_buffer_s>(this, buffer_size_);
        auto* raw    = buffer.get();
        allocated_buffers_.emplace(raw, std::move(buffer));

        *allocatedBuffer = static_cast<IDeckLinkVideoBuffer*>(raw);
        return S_OK;
    } catch (const std::exception& error) {
        getlog("decklink")->error("DeckLink buffer allocation failed: {}", error.what());
    } catch (...) {
        getlog("decklink")->error("DeckLink buffer allocation failed");
    }
    return E_OUTOFMEMORY;
}

void allocator_s::return_buffer(video_buffer_s* buffer)
{
    const std::scoped_lock lock(mutex_);
    auto                   it = allocated_buffers_.find(buffer);
    if (it == allocated_buffers_.end()) {
        getlog("decklink")->error("DeckLink allocator tried to release an unknown buffer");
        return;
    }
    if (shutting_down_) {
        allocated_buffers_.erase(it);
        if (allocated_buffers_.empty()) {
            idle_condition_.notify_all();
        }
        return;
    }
    buffer->clear_upload();
    free_buffers_.emplace_back(std::move(it->second));
    allocated_buffers_.erase(it);
}

decklink_ptr<video_buffer_s> query_upload_video_buffer(IUnknown* source)
{
    if (source == nullptr) {
        return {};
    }

    video_buffer_s* buffer = nullptr;
    if (FAILED(source->QueryInterface(upload_video_buffer_iid(), reinterpret_cast<void**>(&buffer)))) {
        return {};
    }
    return decklink_ptr<video_buffer_s>(buffer, false);
}

void allocator_s::shutdown_and_wait()
{
    std::unique_lock lock(mutex_);
    shutting_down_ = true;
    free_buffers_.clear();
    upload_stream_.reset();
    idle_condition_.wait(lock, [this] { return allocated_buffers_.empty(); });
}

} // namespace miximus::nodes::decklink::detail
