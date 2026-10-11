#pragma once

#include "gpu/transfer/texture_upload.hpp"
#include "media/timed_source_queue.hpp"

namespace miximus::nodes {

enum class source_upload_error_e
{
    none,
    missing,
    wait,
    consume,
    repeat,
    commit
};

struct source_upload_result_s
{
    source_upload_error_e                       error{source_upload_error_e::none};
    gpu::transfer::texture_upload_wait_result_e wait_result{gpu::transfer::texture_upload_wait_result_e::ready};
};

// Payloads retain their SDK-specific metadata and upload lease. ReleaseSource is
// called after a new upload is consumed, before committing the selected ticket.
template <typename Payload, typename ReleaseSource>
source_upload_result_s consume_source_upload(media::timed_source_queue_s<Payload>&    queue,
                                             media::prepared_frame_ticket_s<Payload>& ticket,
                                             ReleaseSource                            release_source)
{
    if (!ticket.frame()) {
        return {.error = source_upload_error_e::missing};
    }
    auto& frame = *ticket.frame();
    auto& info  = frame.payload();
    if (ticket.selection() == media::prepared_frame_selection_e::new_frame) {
        const auto result = info.stream ? info.stream->wait_for_upload_submission(info.upload_id)
                                        : gpu::transfer::texture_upload_wait_result_e::stopped;
        if (result != gpu::transfer::texture_upload_wait_result_e::ready) {
            queue.fail(ticket);
            return {.error = source_upload_error_e::wait, .wait_result = result};
        }
        info.frame = info.stream->select_submitted_upload(info.upload_id);
        if (!info.frame || info.stream->retained_upload_id() != info.upload_id || !frame.mark_ready()) {
            queue.fail(ticket);
            return {.error = source_upload_error_e::consume};
        }
        release_source(info);
    } else if (ticket.selection() == media::prepared_frame_selection_e::repeat) {
        info.frame = info.stream ? info.stream->retained_frame_for(info.upload_id) : nullptr;
        if (!info.frame) {
            queue.fail(ticket);
            return {.error = source_upload_error_e::repeat};
        }
    }
    if (!ticket.await() || !queue.commit(ticket)) {
        return {.error = source_upload_error_e::commit};
    }
    return {};
}

template <typename Payload, typename ReleaseSource>
void cancel_source_upload(media::timed_source_queue_s<Payload>&    queue,
                          media::prepared_frame_ticket_s<Payload>& ticket,
                          ReleaseSource                            release_source)
{
    if (ticket.selection() != media::prepared_frame_selection_e::new_frame || !ticket.frame() ||
        ticket.frame()->readiness() != media::source_frame_readiness_e::submitted) {
        return;
    }
    auto& info = ticket.frame()->payload();
    if (info.stream) {
        info.stream->discard_upload(info.upload_id);
    }
    release_source(info);
    queue.cancel(ticket);
}
} // namespace miximus::nodes
