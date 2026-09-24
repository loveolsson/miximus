#pragma once

#include "gpu/detail/dma_buf_export.hpp"
#include "media_input_pool.hpp"

#include <memory>

namespace miximus::nodes::cef::detail {

// Bounded GPU allocations plus frame-boundary publication. Configuration belongs
// on a control worker; record() belongs on the graph render thread; poll() and
// retire() belong on a transfer worker/transport callback. None waits for a GPU.
class media_input_exports_s
{
    struct state_s;
    struct pending_s;
    std::shared_ptr<state_s> state_;

  public:
    // Retain this owner until AFTER Chromium has shut down. Unproven external
    // reads disable their queue and retain its allocations in this quarantine.
    class quarantine_s
    {
        struct impl_s;
        std::unique_ptr<impl_s> impl_;
        friend class media_input_exports_s;

      public:
        quarantine_s();
        ~quarantine_s();
        quarantine_s(const quarantine_s&)            = delete;
        quarantine_s& operator=(const quarantine_s&) = delete;
    };

    class publication_s
    {
        std::shared_ptr<state_s>   state_;
        std::shared_ptr<pending_s> pending_;
        bool                       committed_{};
        publication_s(std::shared_ptr<state_s> state, std::shared_ptr<pending_s> pending);
        friend class media_input_exports_s;

      public:
        ~publication_s();
        publication_s(const publication_s&)            = delete;
        publication_s& operator=(const publication_s&) = delete;
        // Call only from app->defer_output after the complete graph evaluation
        // has successfully submitted. Destruction without commit revokes delivery.
        void commit();
    };

    class frame_s
    {
        std::shared_ptr<state_s>   state_;
        std::shared_ptr<pending_s> pending_;
        bool                       retired_{true};
        frame_s(std::shared_ptr<state_s> state, std::shared_ptr<pending_s> pending);
        friend class media_input_exports_s;

      public:
        ~frame_s();
        frame_s(const frame_s&)                                        = delete;
        frame_s&                             operator=(const frame_s&) = delete;
        media_input_pool_s::ticket_s         ticket() const;
        const gpu::detail::dma_buf_export_s& image() const;
        int64_t                              timestamp_us() const;
        // Only the transport's proven GPU-copy completion permits true.
        // False/destruction quarantines; it never manufactures completion.
        void retire(bool safe);
    };

    media_input_exports_s(gpu::device_s& device,
                          size_t         depth,
                          quarantine_s&  quarantine,
                          size_t         byte_budget = 128ULL * 1024 * 1024);
    ~media_input_exports_s();
    media_input_exports_s(const media_input_exports_s&)            = delete;
    media_input_exports_s& operator=(const media_input_exports_s&) = delete;
    // Worker-only, allocation is outside the metadata lock. False means old
    // leases must drain first. No overlapping old/new allocation generations.
    bool configure(size_t input, gpu::extent_s extent);
    void invalidate(size_t input);
    std::shared_ptr<publication_s>
    record(size_t input, gpu::recording_s& recording, const gpu::texture_s& source, int64_t timestamp_us);
    // Poll actual producer completion and return at most one consumable lease.
    std::shared_ptr<frame_s>      poll();
    bool                          idle() const;
    bool                          failed() const;
    media_input_pool_s::metrics_s metrics(size_t input) const;
};
} // namespace miximus::nodes::cef::detail
