#include "gpu/detail/dma_buf_copy.hpp"
#include "gpu/detail/dma_buf_export.hpp"
#include "gpu/device.hpp"
#include "gpu/tests/color_compare.hpp"
#include "logger/logger.hpp"
#include "nodes/cef/detail/media_input_exports.hpp"
#include "nodes/cef/detail/media_input_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace miximus::nodes::cef::detail { namespace {
using namespace std::chrono_literals;

TEST(media_input_vulkan, EightInputsReuseExportSlotsAfterCompletedGpuCopies)
{
    if (!spdlog::get("gpu")) {
        logger::init_loggers(spdlog::level::warn);
    }

    gpu::device_options_s options;
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const auto* uuid = std::getenv("MIXIMUS_VULKAN_DEVICE")) {
        options.device_uuid = uuid;
    }

    options.external_image_import = true;
    gpu::device_s producer(options);
    gpu::device_s consumer(options);
    ASSERT_TRUE(producer.external_image_import_support().enabled);
    auto                            producer_context = producer.create_recording_context(1);
    auto                            consumer_context = consumer.create_recording_context(1);
    constexpr gpu::extent_s         extent{.width = 640, .height = 360};
    auto                            source      = producer.create_texture(extent);
    auto                            destination = consumer.create_texture(extent);
    gpu::detail::color_comparison_s compare(destination, MIXIMUS_CEF_COMPARE_SHADER);
    auto                            counters = consumer.create_buffer(8, gpu::host_access_e::read_write);
    gpu::draw_s                     conversion;
    conversion.compositing = gpu::compositing_e::replace;

    for (size_t depth : {1U, 2U, 3U}) {
        media_input_pool_s                                          pool(depth);
        std::vector<std::unique_ptr<gpu::detail::dma_buf_export_s>> exports;
        for (size_t index = 0; index < depth * media_input_pool_s::INPUT_COUNT; ++index) {
            exports.push_back(std::make_unique<gpu::detail::dma_buf_export_s>(producer, extent));
            EXPECT_GT(exports.back()->allocation_bytes(), 0U);
        }

        // Fill all slots before consuming, then change every slot's pixels on
        // every round. Static textures cannot catch premature reuse/stale reads.
        for (size_t round = 0; round < 4; ++round) {
            {
                auto abandoned = producer_context.try_record();
                ASSERT_TRUE(abandoned);
                abandoned->clear(source, {1, 1, 1, 1});
                exports.front()->copy(*abandoned, source, conversion);
                // No native submission: first-use/reacquire ownership must not
                // change, and the next recording must still work.
            }

            std::vector<media_input_pool_s::ticket_s> tickets;
            std::vector<std::array<float, 4>>         colors;
            for (size_t input = 0; input < media_input_pool_s::INPUT_COUNT; ++input) {
                for (size_t slot = 0; slot < depth; ++slot) {
                    const auto ticket = pool.acquire(input);
                    if (!ticket) {
                        FAIL() << "Export slot unavailable";
                        return;
                    }

                    tickets.push_back(*ticket);
                    const std::array<float, 4> color{
                        float((round + slot) % 4) / 4, float(input) / 8, float(round) / 4, 1};
                    colors.push_back(color);
                    auto& exported = *exports[(input * depth) + slot];
                    auto  record   = producer_context.try_record();
                    ASSERT_TRUE(record);
                    record->clear(source, color);
                    exported.copy(*record, source, conversion);
                    record->on_submitted([&pool, ticket = *ticket](const gpu::completion_s& /* completion */) {
                        EXPECT_TRUE(pool.publish(ticket));
                    });
                    ASSERT_EQ(record->submit().wait(5s), gpu::wait_result_e::ready);
                    ASSERT_TRUE(pool.producer_finished(*ticket));
                }

                EXPECT_FALSE(pool.acquire(input));
            }

            for (size_t index = 0; index < tickets.size(); ++index) {
                const auto ticket = tickets[index];
                ASSERT_TRUE(pool.begin_consume(ticket));
                auto& exported = *exports[(ticket.input * depth) + ticket.slot];
                auto  record   = consumer_context.try_record();
                ASSERT_TRUE(record);
                // Host waits are test orchestration, never CPU pixel transfer.
                // Producer completion above precedes any import/read of its FD.
                const auto completion =
                    gpu::detail::dma_buf_copy_s::submit(*record, exported.descriptor(), destination, conversion, 500ms);
                ASSERT_EQ(completion.wait(5s), gpu::wait_result_e::ready);
                record.reset();
                ASSERT_TRUE(pool.consumer_finished(ticket));
                std::ranges::fill(counters.writable_bytes(), std::byte{});
                record = consumer_context.try_record();
                ASSERT_TRUE(record);
                compare.record(*record, destination, counters, colors[index], 0.005F);
                ASSERT_EQ(record->submit().wait(5s), gpu::wait_result_e::ready);
                record.reset();
                std::array<uint32_t, 2> errors{};
                const auto              bytes = counters.readable_bytes();
                std::memcpy(errors.data(), bytes.data(), sizeof(errors));
                EXPECT_EQ(errors[0], 0U) << "depth=" << depth << " input=" << ticket.input << " round=" << round;
            }
        }

        for (size_t input = 0; input < media_input_pool_s::INPUT_COUNT; ++input) {
            EXPECT_EQ(pool.metrics(input).occupied, 0U);
            EXPECT_EQ(pool.metrics(input).high_water, depth);
        }
    }

    EXPECT_EQ(producer.validation_errors(), 0U);
    EXPECT_EQ(consumer.validation_errors(), 0U);
}

class export_queue_test : public testing::Test
{
  public:
    std::unique_ptr<gpu::device_s>                       device;
    std::unique_ptr<media_input_exports_s::quarantine_s> quarantine;
    std::unique_ptr<media_input_exports_s>               queue;
    gpu::texture_s                                       source;
    std::unique_ptr<gpu::recording_context_s>            context;
    void                                                 SetUp() override
    {
        if (!spdlog::get("gpu")) {
            logger::init_loggers(spdlog::level::warn);
        }

        gpu::device_options_s options;
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation            = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        options.external_image_import = true;
        device                        = std::make_unique<gpu::device_s>(options);
        quarantine                    = std::make_unique<media_input_exports_s::quarantine_s>();
        queue                         = std::make_unique<media_input_exports_s>(*device, 1, *quarantine);
        ASSERT_TRUE(queue->configure(0, {16, 16}));
        source  = device->create_texture({.width = 16, .height = 16});
        context = std::make_unique<gpu::recording_context_s>(device->create_recording_context(1));
    }

    void TearDown() override { EXPECT_EQ(device->validation_errors(), 0U); }
    std::shared_ptr<media_input_exports_s::publication_s> record(std::unique_ptr<gpu::recording_s>& commands) const
    {
        commands = context->try_record();
        commands->clear(source, {0.1F, 0.2F, 0.3F, 0.5F});
        return queue->record(0, *commands, source, 12345);
    }

    static void finish(std::unique_ptr<gpu::recording_s>& commands)
    {
        EXPECT_EQ(commands->submit().wait(5s), gpu::wait_result_e::ready);
        commands.reset();
    }
};

TEST_F(export_queue_test, InactiveExportsAreFreedOnlyAfterAllLeasesRetire)
{
    std::unique_ptr<gpu::recording_s> commands;
    auto                              publication = record(commands);
    publication->commit();
    EXPECT_FALSE(queue->release(0));
    finish(commands);
    auto frame = queue->poll();
    ASSERT_TRUE(frame);
    queue->invalidate(0);
    EXPECT_FALSE(queue->release(0));
    EXPECT_GT(queue->allocated_bytes(), 0U);
    frame->retire(true);
    // Retired frame/publication handles still own their backing allocation.
    EXPECT_FALSE(queue->release(0));
    frame.reset();
    publication.reset();
    ASSERT_TRUE(queue->release(0));
    EXPECT_EQ(queue->allocated_bytes(), 0U);
    ASSERT_TRUE(queue->configure(0, {32, 16}));
    EXPECT_GT(queue->allocated_bytes(), 0U);
}

TEST_F(export_queue_test, NativeSubmissionAloneCannotPublishAnIncompleteGraphFrame)
{
    std::unique_ptr<gpu::recording_s> commands;
    auto                              publication = record(commands);
    ASSERT_TRUE(publication);
    EXPECT_FALSE(queue->poll());
    finish(commands);
    EXPECT_FALSE(queue->poll());
    EXPECT_EQ(queue->metrics(0).occupied, 1U);
    publication->commit();
    auto frame = queue->poll();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->timestamp_us(), 12345);
    EXPECT_EQ(frame->ticket().input, 0U);
    auto next = context->try_record();
    EXPECT_FALSE(queue->record(0, *next, source, 2));
    next.reset();
    frame->retire(true); // Transport rejected before external access.
    EXPECT_TRUE(queue->idle());
}

TEST_F(export_queue_test, AbandonedRecordingReturnsCapacityWithoutPublishing)
{
    std::unique_ptr<gpu::recording_s> commands;
    auto                              publication = record(commands);
    publication.reset();
    EXPECT_FALSE(queue->idle());
    commands.reset();
    EXPECT_FALSE(queue->poll());
    EXPECT_TRUE(queue->idle());
    EXPECT_TRUE(queue->configure(0, {32, 16}));
}

TEST_F(export_queue_test, SubmittedThenAbortedFrameDrainsProducerWithoutDelivery)
{
    std::unique_ptr<gpu::recording_s> commands;
    auto                              publication = record(commands);
    finish(commands);
    publication.reset();
    EXPECT_FALSE(queue->poll());
    EXPECT_TRUE(queue->idle());
    EXPECT_FALSE(queue->failed());
}

TEST_F(export_queue_test, ResizeCannotReplaceAConsumerOrRetainedAllocation)
{
    std::unique_ptr<gpu::recording_s> commands;
    auto                              publication = record(commands);
    finish(commands);
    publication->commit();
    auto frame = queue->poll();
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->current());
    EXPECT_EQ(frame->ticket().generation, queue->generation(0));
    queue->invalidate(0);
    EXPECT_FALSE(frame->current());
    EXPECT_GT(queue->generation(0), frame->ticket().generation);
    EXPECT_FALSE(queue->configure(0, {32, 16}));
    frame->retire(true);
    EXPECT_FALSE(queue->configure(0, {32, 16})); // The client still holds the allocation.
    frame.reset();
    publication.reset();
    EXPECT_TRUE(queue->configure(0, {32, 16}));
}

TEST_F(export_queue_test, ForgottenConsumerQuarantinesAcrossQueueDestruction)
{
    auto               lease    = std::make_shared<int>(1);
    std::weak_ptr<int> retained = lease;
    queue = std::make_unique<media_input_exports_s>(*device, 1, *quarantine, 128ULL * 1024 * 1024, lease);
    lease.reset();
    ASSERT_TRUE(queue->configure(0, {16, 16}));
    std::unique_ptr<gpu::recording_s> commands;
    auto                              publication = record(commands);
    finish(commands);
    publication->commit();
    auto frame = queue->poll();
    ASSERT_TRUE(frame);
    const int fd = frame->image().descriptor().fd;
    frame.reset();
    publication.reset();
    EXPECT_TRUE(queue->failed());
    EXPECT_FALSE(queue->configure(0, {16, 16}));
    queue.reset();
    EXPECT_NE(fcntl(fd, F_GETFD), -1); // Quarantine outlives the producer queue.
    EXPECT_FALSE(retained.expired());  // Its shared admission charge must survive too.
    quarantine.reset();
    EXPECT_TRUE(retained.expired());
}

TEST_F(export_queue_test, EightInputsRemainBoundedAndByteBudgetRejectsOversizeBeforeAllocation)
{
    for (size_t input = 1; input < 8; ++input) {
        ASSERT_TRUE(queue->configure(input, {16, 16}));
    }

    auto commands = context->try_record();
    commands->clear(source, {0, 0, 0, 1});
    std::vector<std::shared_ptr<media_input_exports_s::publication_s>> publications;
    for (size_t input = 0; input < 8; ++input) {
        auto publication = queue->record(input, *commands, source, static_cast<int64_t>(input));
        ASSERT_TRUE(publication);
        commands->on_submitted([publication](const gpu::completion_s&) { publication->commit(); });
        publications.push_back(publication);
        EXPECT_FALSE(queue->record(input, *commands, source, 99));
    }

    finish(commands);
    uint32_t received{};
    while (auto frame = queue->poll()) {
        received |= 1U << frame->ticket().input;
        frame->retire(true);
    }

    EXPECT_EQ(received, 255U);
    EXPECT_TRUE(queue->idle());
    media_input_exports_s limited(*device, 1, *quarantine, 1024);
    EXPECT_THROW(limited.configure(0, {640, 360}), std::runtime_error);
    EXPECT_TRUE(limited.idle());
}

TEST_F(export_queue_test, CapacityFailureCanRecoverAfterAnotherInputReleasesItsAllocations)
{
    const auto            capacity = queue->allocated_bytes();
    media_input_exports_s limited(*device, 1, *quarantine, capacity);
    ASSERT_TRUE(limited.configure(0, {16, 16}));
    EXPECT_THROW(limited.configure(1, {16, 16}), media_input_exports_s::capacity_error_s);
    EXPECT_EQ(limited.allocated_bytes(), capacity);
    limited.invalidate(0);
    ASSERT_TRUE(limited.release(0));
    ASSERT_TRUE(limited.configure(1, {16, 16}));
    EXPECT_EQ(limited.allocated_bytes(), capacity);
    EXPECT_THROW(limited.configure(2, {4097, 16}), std::invalid_argument);
}

TEST_F(export_queue_test, PendingFrameDecisionCannotBeOvertakenByANewerFrame)
{
    queue = std::make_unique<media_input_exports_s>(*device, 2, *quarantine);
    ASSERT_TRUE(queue->configure(0, {16, 16}));
    std::unique_ptr<gpu::recording_s> commands;
    auto                              first = record(commands);
    finish(commands);
    commands    = context->try_record();
    auto second = queue->record(0, *commands, source, 23456);
    finish(commands);
    second->commit();
    EXPECT_FALSE(queue->poll());
    first->commit();
    auto frame = queue->poll();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->timestamp_us(), 12345);
    frame->retire(true);
    frame = queue->poll();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->timestamp_us(), 23456);
    frame->retire(true);
}

}} // namespace miximus::nodes::cef::detail
