#include "allocator.hpp"

#include "logger/logger.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <exception>
#include <memory>
#include <utility>

namespace miximus::nodes::decklink::detail {
namespace {
constexpr size_t MAX_ALLOCATIONS = 6;
}

ULONG video_buffer_s::Release()
{
    const ULONG count = --ref_count_;
    if (count == 0) {
        allocator_->return_buffer(this);
    }
    return count;
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

HRESULT allocator_s::AllocateVideoBuffer(IDeckLinkVideoBuffer** allocatedBuffer)
{
    if (allocatedBuffer == nullptr) {
        return E_POINTER;
    }
    *allocatedBuffer = nullptr;

    try {
        std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
        buffer_ptr_t                                            reusable;
        {
            const std::scoped_lock lock(mutex_);
            stream = shutting_down_ ? nullptr : upload_stream_;
            if (stream && !free_buffers_.empty()) {
                reusable = std::move(free_buffers_.front());
                free_buffers_.pop_front();
            }
        }
        if (!stream) {
            return E_OUTOFMEMORY;
        }

        // Returned SDK objects still pin their submitted upload lease. Release it
        // before asking the stream for replacement memory to avoid pool starvation.
        if (reusable) {
            reusable->clear_upload();
        }

        // This callback runs on a DeckLink SDK thread, never the render thread. A
        // short wait allows the GL upload worker to lazily allocate a backend slot.
        auto upload = stream->acquire_for(std::chrono::milliseconds(100));
        if (!upload) {
            if (reusable) {
                const std::scoped_lock lock(mutex_);
                free_buffers_.emplace_front(std::move(reusable));
            }
            return E_OUTOFMEMORY;
        }

        const std::scoped_lock lock(mutex_);
        if (!reusable && !free_buffers_.empty()) {
            reusable = std::move(free_buffers_.front());
            free_buffers_.pop_front();
            reusable->clear_upload();
        }
        if (reusable) {
            auto* raw = reusable.get();
            raw->set_upload(std::move(*upload));
            raw->reset_ref_count();
            allocated_buffers_.emplace(raw, std::move(reusable));
            *allocatedBuffer = static_cast<IDeckLinkVideoBuffer*>(raw);
            return S_OK;
        }

        if (allocated_buffers_.size() + free_buffers_.size() >= MAX_ALLOCATIONS) {
            return E_OUTOFMEMORY;
        }

        getlog("decklink")->debug("Allocating upload-backed DeckLink buffer, current count {}", ++allocations_g);
        auto  buffer = std::make_unique<video_buffer_s>(this, std::move(*upload));
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
        --allocations_g;
        allocated_buffers_.erase(it);
        return;
    }
    free_buffers_.emplace_back(std::move(it->second));
    allocated_buffers_.erase(it);
}

uint64_t allocator_s::upload_version(IDeckLinkVideoBuffer* buffer)
{
    const std::scoped_lock lock(mutex_);
    const auto             it = std::ranges::find_if(allocated_buffers_, [buffer](const auto& entry) {
        return static_cast<IDeckLinkVideoBuffer*>(entry.first) == buffer;
    });
    if (it == allocated_buffers_.end()) {
        return 0;
    }
    return it->second->upload_version();
}

bool allocator_s::submit_upload(IDeckLinkVideoBuffer* buffer)
{
    const std::scoped_lock lock(mutex_);
    const auto             it = std::ranges::find_if(allocated_buffers_, [buffer](const auto& entry) {
        return static_cast<IDeckLinkVideoBuffer*>(entry.first) == buffer;
    });
    if (it == allocated_buffers_.end()) {
        return false;
    }
    it->second->submit_upload();
    return true;
}

size_t allocator_s::destroy_free_buffers()
{
    const std::scoped_lock lock(mutex_);
    shutting_down_ = true;
    allocations_g -= free_buffers_.size();
    free_buffers_.clear();
    upload_stream_.reset();
    getlog("decklink")->debug("Destroying free DeckLink buffers, current count {}", allocations_g.load());
    return allocated_buffers_.size();
}

} // namespace miximus::nodes::decklink::detail
