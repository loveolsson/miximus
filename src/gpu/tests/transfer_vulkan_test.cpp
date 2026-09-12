#include "gpu/device.hpp"
#include "gpu/drawing.hpp"
#include "gpu/transfer/detail/frame_staging.hpp"
#include "gpu/transfer/detail/transfer_worker.hpp"
#include "gpu/transfer/texture_readback.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "logger/logger.hpp"
#ifdef MIXIMUS_TEST_DECKLINK
#include "nodes/decklink/detail/allocator.hpp"
#include "nodes/decklink/detail/output_video_buffer.hpp"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <thread>
using namespace miximus;
using namespace gpu;
using namespace gpu::transfer;
using namespace std::chrono_literals;
namespace {

class transfer_vulkan : public testing::Test
{
    std::unique_ptr<device_s> device_;
    static inline bool        disable_cuda_{};
    static inline bool        log_debug_{};

  public:
    static void configure(bool disable_cuda, bool log_debug)
    {
        disable_cuda_ = disable_cuda;
        log_debug_    = log_debug;
    }

  protected:
    device_s& gpu() const { return *device_; }
    void      SetUp() override
    {
        static const bool initialized = [] {
            logger::init_loggers(log_debug_ ? spdlog::level::debug : spdlog::level::info);
            return true;
        }();
        (void)initialized;
        device_ = std::make_unique<device_s>(
            // Test options are read from an environment that the test does not modify.
            // NOLINTNEXTLINE(concurrency-mt-unsafe)
            device_options_s{.validation   = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr,
                             .disable_cuda = disable_cuda_});
    }

    void TearDown() override
    {
        if (device_) {
            EXPECT_EQ(device_->validation_errors(), 0);
        }
    }

    template <class F>
    auto until(F fn)
    {
        auto deadline = std::chrono::steady_clock::now() + 3s;
        auto result   = fn();
        while (!result && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
            result = fn();
        }

        return result;
    }
};

TEST_F(transfer_vulkan, ExactUploadSelectionRetainsOldFrameAndProducesPaddedReadback)
{
    texture_upload_service_s   uploads(gpu(), 1 << 20);
    texture_readback_service_s downloads(gpu(), 1 << 20);
    const host_frame_layout_s  layout{
         .image_dimensions  = {3, 2},
         .pixel_format      = host_pixel_format_e::rgba_u8,
         .row_stride_bytes  = 20,
         .buffer_size_bytes = 40
    };

    auto input =
        uploads.create_stream({.host_layout = layout, .max_slots = 2, .initial_slots = 2, .generate_mip_maps = false});
    auto readback_layout          = layout;
    readback_layout.memory_access = host_memory_access_e::read_only;
    auto output = downloads.create_stream({.host_layout = readback_layout, .max_slots = 1, .initial_slots = 1});
    ASSERT_TRUE(output->wait_for_initial_slots(3s));
    auto first  = input->acquire_upload_buffer_for(3s);
    auto second = input->acquire_upload_buffer_for(3s);
    if (!first.has_value()) {
        ADD_FAILURE() << "Expected first to hold a value";
        return;
    }
    if (!second.has_value()) {
        ADD_FAILURE() << "Expected second to hold a value";
        return;
    }
    const auto first_id  = first->upload_id();
    const auto second_id = second->upload_id();
    std::ranges::fill(first->writable_host_bytes(), std::byte{17});
    std::ranges::fill(second->writable_host_bytes(), std::byte{89});
    ASSERT_TRUE(second->submit());
    ASSERT_TRUE(first->submit());
    first.reset();
    second.reset();
    ASSERT_EQ(input->wait_for_upload(first_id), texture_upload_wait_result_e::ready);
    auto selected = input->select_completed_upload(first_id);
    ASSERT_TRUE(selected);
    auto target = output->try_acquire_render_target();
    if (!target.has_value()) {
        ADD_FAILURE() << "Expected target to hold a value";
        return;
    }

    auto commands = until([&] { return gpu().try_record(); });
    ASSERT_TRUE(commands);
    draw_texture(
        *commands, selected->texture(), target->texture(), {}, 1, color_operation_e::none, compositing_e::replace);
    target->set_program_target_time(utils::flicks{1234});
    target->submit(commands->submit());
    commands.reset();
    target.reset();
    auto frame = until([&] { return output->try_consume_oldest(); });
    if (!frame.has_value()) {
        ADD_FAILURE() << "Expected frame to hold a value";
        return;
    }
    EXPECT_EQ(frame->program_target_time(), utils::flicks{1234});
    auto bytes = frame->readable_host_bytes();
    for (size_t y = 0; y < 2; ++y) {
        for (size_t x = 0; x < 12; ++x) {
            EXPECT_EQ(bytes[(y * 20) + x], std::byte{17});
        }
    }

    EXPECT_FALSE(output->try_acquire_render_target());
    input->discard_upload(first_id);
    input->discard_upload(second_id);
    // The selected frame still owns its input slot, even after source retirement.
    auto other = until([&] { return input->try_acquire_upload_buffer(); });
    if (!other.has_value()) {
        ADD_FAILURE() << "Expected other to hold a value";
        return;
    }
    EXPECT_FALSE(input->try_acquire_upload_buffer());
    const auto replacement_id = other->upload_id();
    ASSERT_TRUE(other->submit());
    other.reset();
    ASSERT_EQ(input->wait_for_upload(replacement_id), texture_upload_wait_result_e::ready);
    auto replacement = input->select_completed_upload(replacement_id);
    ASSERT_TRUE(replacement);
    EXPECT_FALSE(input->try_acquire_upload_buffer());
    selected.reset();
    auto released = until([&] { return input->try_acquire_upload_buffer(); });
    EXPECT_TRUE(released);
    frame.reset();
    EXPECT_TRUE(output->try_acquire_render_target());
}

TEST_F(transfer_vulkan, FifoUploadsStartWhileRenderingAndRetainLaterCompletedFrames)
{
    texture_upload_service_s  uploads(gpu(), 1 << 20);
    const host_frame_layout_s layout{
        .image_dimensions = {1, 1},
          .pixel_format = host_pixel_format_e::rgba_u8, .buffer_size_bytes = 4
    };
    auto input     = uploads.create_stream({.host_layout = layout, .max_slots = 3, .initial_slots = 3});
    auto first     = input->acquire_upload_buffer_for(3s);
    auto second    = input->acquire_upload_buffer_for(3s);
    auto discarded = input->acquire_upload_buffer_for(3s);
    if (!first || !second || !discarded) {
        FAIL() << "Expected three preallocated FIFO slots";
    }
    const auto first_id  = first->upload_id();
    const auto second_id = second->upload_id();
    auto       held      = gpu().try_record();
    ASSERT_TRUE(held);
    std::ranges::fill(first->writable_host_bytes(), std::byte{11});
    std::ranges::fill(second->writable_host_bytes(), std::byte{22});
    std::ranges::fill(discarded->writable_host_bytes(), std::byte{33});
    ASSERT_TRUE(first->submit(upload_ownership_e::queued_frame));
    ASSERT_TRUE(second->submit(upload_ownership_e::queued_frame));
    ASSERT_TRUE(discarded->submit(upload_ownership_e::queued_frame));
    ASSERT_TRUE(until([&] { return input->latest_completed_upload_id() == discarded->upload_id(); }));
    auto selected = input->select_completed_upload(first_id);
    ASSERT_TRUE(selected);
    EXPECT_EQ(input->retained_upload_id(), first_id);
    discarded.reset(); // Eviction must reclaim precisely this frame's slot.
    auto replacement = input->acquire_upload_buffer_for(3s);
    ASSERT_TRUE(replacement);
    selected = input->select_completed_upload(second_id);
    ASSERT_TRUE(selected);
    EXPECT_EQ(input->retained_upload_id(), second_id);
}

TEST_F(transfer_vulkan, AbandonedOutputTargetCannotBeReusedDuringRecording)
{
    texture_readback_service_s downloads(gpu(), 1 << 20);
    auto                       output = downloads.create_stream({
                              .host_layout   = {.image_dimensions  = {4, 4},
                                                .pixel_format      = host_pixel_format_e::rgba_u8,
                                                .buffer_size_bytes = 64,
                                                .memory_access     = host_memory_access_e::read_only},
                              .max_slots     = 1,
                              .initial_slots = 1
    });
    ASSERT_TRUE(output->wait_for_initial_slots(3s));
    auto target = output->try_acquire_render_target();
    if (!target.has_value()) {
        ADD_FAILURE() << "Expected target to hold a value";
        return;
    }

    auto commands = until([&] { return gpu().try_record(); });
    ASSERT_TRUE(commands);
    EXPECT_THROW(target->submit({}), std::invalid_argument);
    target->texture()->clear(*commands);
    target.reset();
    EXPECT_FALSE(output->try_acquire_render_target());
    commands.reset();
    EXPECT_TRUE(output->try_acquire_render_target());
}

// Hold the first retry while another producer adds work. Older pending
// completions must still be serviced before that new arrival.
TEST_F(transfer_vulkan, PendingTransfersAreRetriedBeforeNewArrivals)
{
    struct task_s
    {
        size_t id{};
        bool   pending{};
    };

    struct worker_s : gpu::transfer::detail::transfer_worker_s<worker_s, task_s>
    {
        std::mutex              mutex;
        std::condition_variable condition;
        bool                    retry_entered{};
        bool                    resume{};
        std::vector<size_t>     completed;
        explicit worker_s(device_s& device)
            : transfer_worker_s(device, 0)
        {
        }

        static bool is_resource_task(const task_s&) noexcept { return false; }

        bool process_task(task_s& task)
        {
            if (!std::exchange(task.pending, true)) {
                return false;
            }

            std::unique_lock lock(mutex);
            if (task.id == 0) {
                retry_entered = true;
                condition.notify_all();
                condition.wait(lock, [&] { return resume; });
            }

            completed.push_back(task.id);
            return true;
        }
    };

    auto             worker = std::make_shared<worker_s>(gpu());
    constexpr size_t count  = 32;
    for (size_t i = 0; i < count; ++i) {
        worker->enqueue({.id = i, .pending = false});
    }

    worker->start();
    {
        std::unique_lock lock(worker->mutex);
        EXPECT_TRUE(worker->condition.wait_for(lock, 3s, [&] { return worker->retry_entered; }));
        worker->enqueue({.id = count, .pending = true});
        worker->resume = true;
    }

    worker->condition.notify_all();
    worker->stop();
    ASSERT_EQ(worker->completed.size(), count + 1);
    for (size_t i = 0; i <= count; ++i) {
        EXPECT_EQ(worker->completed[i], i);
    }
}

TEST_F(transfer_vulkan, ResourceAllocationDoesNotBlockTransferProgress)
{
    enum class task_e
    {
        resource,
        transfer
    };
    struct worker_s : gpu::transfer::detail::transfer_worker_s<worker_s, task_e>
    {
        std::mutex              mutex;
        std::condition_variable condition;
        bool                    allocation_entered{};
        bool                    release_allocation{};
        bool                    transfer_completed{};

        explicit worker_s(device_s& device)
            : transfer_worker_s(device, 0)
        {
        }
        static bool is_resource_task(task_e task) { return task == task_e::resource; }
        bool        process_task(task_e task)
        {
            std::unique_lock lock(mutex);
            if (task == task_e::resource) {
                allocation_entered = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release_allocation; });
            } else {
                transfer_completed = true;
                condition.notify_all();
            }
            return true;
        }
    };
    auto worker = std::make_shared<worker_s>(gpu());
    worker->start();
    worker->enqueue(task_e::resource);
    {
        std::unique_lock lock(worker->mutex);
        EXPECT_TRUE(worker->condition.wait_for(lock, 3s, [&] { return worker->allocation_entered; }));
        worker->enqueue(task_e::transfer);
        EXPECT_TRUE(worker->condition.wait_for(lock, 3s, [&] { return worker->transfer_completed; }));
        worker->release_allocation = true;
    }
    worker->condition.notify_all();
    worker->stop();
}

TEST_F(transfer_vulkan, SubmittedUploadsChainThroughConversionAndReadbackWithoutCpuCompletionWaits)
{
    texture_upload_service_s   uploads(gpu(), 1 << 20);
    texture_readback_service_s downloads(gpu(), 1 << 20);
    const host_frame_layout_s  layout{
         .image_dimensions = {4, 4},
           .pixel_format = host_pixel_format_e::rgba_u8, .buffer_size_bytes = 64
    };
    auto input                  = uploads.create_stream({.host_layout         = layout,
                                                         .max_slots           = 2,
                                                         .initial_slots       = 2,
                                                         .generate_mip_maps   = false,
                                                         .conversion_sampling = sampling_e::linear});
    auto output_layout          = layout;
    output_layout.memory_access = host_memory_access_e::read_only;
    auto output                 = downloads.create_stream(
        {.host_layout = output_layout, .max_slots = 1, .initial_slots = 1, .conversion_sampling = sampling_e::linear});
    ASSERT_TRUE(output->wait_for_initial_slots(3s));
    std::shared_ptr<texture_s> conversion;
    for (unsigned value = 1; value <= 32; ++value) {
        auto lease = input->acquire_upload_buffer_for(3s);
        ASSERT_TRUE(lease);
        const auto id = lease->upload_id();
        std::ranges::fill(lease->writable_host_bytes(), std::byte(value));
        ASSERT_TRUE(lease->submit(upload_ownership_e::queued_frame));
        ASSERT_EQ(input->wait_for_upload_submission(id), texture_upload_wait_result_e::ready);
        auto frame = input->select_submitted_upload(id);
        ASSERT_TRUE(frame);
        ASSERT_TRUE(frame->conversion_texture());
        if (conversion) {
            EXPECT_EQ(frame->conversion_texture(), conversion);
        }
        conversion  = frame->conversion_texture();
        auto target = until([&] { return output->try_acquire_render_target(); });
        ASSERT_TRUE(target);
        ASSERT_NE(target->conversion_texture(), nullptr);
        auto commands = until([&] { return gpu().try_record(); });
        ASSERT_TRUE(commands);
        commands->wait_for(frame->upload_completion());
        draw_texture(
            *commands, frame->texture(), conversion.get(), {}, 1, color_operation_e::none, compositing_e::replace);
        draw_texture(*commands,
                     conversion.get(),
                     target->conversion_texture(),
                     {},
                     1,
                     color_operation_e::none,
                     compositing_e::replace);
        draw_texture(*commands,
                     target->conversion_texture(),
                     target->texture(),
                     {},
                     1,
                     color_operation_e::none,
                     compositing_e::replace);
        target->submit(commands->submit());
        target.reset();
        commands.reset();
        lease.reset();
        auto result = until([&] { return output->try_consume_oldest(); });
        ASSERT_TRUE(result);
        for (const auto byte : result->readable_host_bytes()) {
            EXPECT_EQ(byte, std::byte(value));
        }
        ASSERT_EQ(input->wait_for_upload(id), texture_upload_wait_result_e::ready);
    }
}

TEST_F(transfer_vulkan, ConcurrentReadbackStreamsPreserveFrameOrderAndPixels)
{
    texture_readback_service_s                                           downloads(gpu(), 1 << 20);
    constexpr size_t                                                     stream_count = 2;
    constexpr size_t                                                     slots        = 8;
    constexpr size_t                                                     rounds       = 8;
    std::array<std::shared_ptr<texture_readback_stream_s>, stream_count> streams;
    for (auto& stream : streams) {
        stream = downloads.create_stream({
            .host_layout   = {.image_dimensions  = {8, 8},
                              .pixel_format      = host_pixel_format_e::rgba_u8,
                              .buffer_size_bytes = 256,
                              .memory_access     = host_memory_access_e::read_only},
            .max_slots     = slots,
            .initial_slots = slots
        });
        ASSERT_TRUE(stream->wait_for_initial_slots(3s));
    }

    for (size_t round = 0; round < rounds; ++round) {
        for (size_t frame = 0; frame < slots; ++frame) {
            for (size_t stream = 0; stream < stream_count; ++stream) {
                auto target = until([&] { return streams.at(stream)->try_acquire_render_target(); });
                if (!target.has_value()) {
                    ADD_FAILURE() << "Expected target to hold a value";
                    return;
                }

                auto commands = until([&] { return gpu().try_record(); });
                ASSERT_TRUE(commands);
                const auto sequence = (round * slots) + frame + 1;
                commands->clear(*target->texture(), {float(sequence) / 255, float(stream), 0, 1});
                target->set_program_target_time(utils::flicks{static_cast<int64_t>(sequence)});
                auto ready = commands->submit();
                commands.reset();
                target->submit(std::move(ready));
            }
        }

        for (size_t frame = 0; frame < slots; ++frame) {
            for (size_t stream = 0; stream < stream_count; ++stream) {
                auto result = until([&] { return streams.at(stream)->try_consume_oldest(); });
                if (!result.has_value()) {
                    ADD_FAILURE() << "Expected result to hold a value";
                    return;
                }
                const auto sequence = (round * slots) + frame + 1;
                EXPECT_EQ(result->program_target_time(), utils::flicks{static_cast<int64_t>(sequence)});

                const auto bytes = result->readable_host_bytes();
                for (size_t pixel = 0; pixel < 64; ++pixel) {
                    EXPECT_EQ(bytes[pixel * 4], std::byte(sequence));
                    EXPECT_EQ(bytes[(pixel * 4) + 1], stream ? std::byte{255} : std::byte{});
                    EXPECT_EQ(bytes[(pixel * 4) + 3], std::byte{255});
                }
            }
        }
    }
}

TEST_F(transfer_vulkan, PackedFramesUseBuffersAndPreserveRowPadding)
{
    texture_upload_service_s   uploads(gpu(), 1 << 20);
    texture_readback_service_s downloads(gpu(), 1 << 20);
    const host_frame_layout_s  layout{
         .image_dimensions  = {6, 2},
         .pixel_format      = host_pixel_format_e::v210,
         .row_stride_bytes  = 32,
         .buffer_size_bytes = 64,
    };

    auto input                    = uploads.create_stream({.host_layout = layout, .max_slots = 1, .initial_slots = 1});
    auto readback_layout          = layout;
    readback_layout.memory_access = host_memory_access_e::read_only;
    auto output = downloads.create_stream({.host_layout = readback_layout, .max_slots = 1, .initial_slots = 1});
    ASSERT_TRUE(output->wait_for_initial_slots(3s));
    auto upload = input->acquire_upload_buffer_for(3s);
    if (!upload.has_value()) {
        ADD_FAILURE() << "Expected upload to hold a value";
        return;
    }
    auto bytes = upload->writable_host_bytes();
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(i);
    }

    const auto id = upload->upload_id();
    ASSERT_TRUE(upload->submit());
    upload.reset();
    ASSERT_EQ(input->wait_for_upload(id), texture_upload_wait_result_e::ready);
    auto selected = input->select_completed_upload(id);
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->texture(), nullptr);
    EXPECT_EQ(selected->layout().row_stride_bytes, 32);
    ASSERT_EQ(selected->buffer().size(), 64);
    auto target = output->try_acquire_render_target();
    if (!target.has_value()) {
        ADD_FAILURE() << "Expected target to hold a value";
        return;
    }
    EXPECT_EQ(target->texture(), nullptr);
    ASSERT_EQ(target->buffer().size(), 64);

    auto commands = until([&] { return gpu().try_record(); });
    ASSERT_TRUE(commands);
    commands->copy(selected->buffer(), target->buffer(), 64);
    target->submit(commands->submit());
    commands.reset();
    target.reset();
    auto frame = until([&] { return output->try_consume_oldest(); });
    if (!frame.has_value()) {
        ADD_FAILURE() << "Expected frame to hold a value";
        return;
    }

    const auto result = frame->readable_host_bytes();
    ASSERT_EQ(result.size(), 64);
    for (size_t i = 0; i < result.size(); ++i) {
        EXPECT_EQ(result[i], static_cast<std::byte>(i));
    }

    EXPECT_FALSE(output->try_acquire_render_target());
    frame.reset();
    EXPECT_TRUE(output->try_acquire_render_target());
}

TEST_F(transfer_vulkan, MappedDeckLinkAlignmentAndBudgetAreRespected)
{
    texture_upload_service_s uploads(gpu(), 1 << 20);
    auto                     input = uploads.create_stream({
                            .host_layout   = {.image_dimensions        = {48, 4},
                                              .pixel_format            = host_pixel_format_e::v210,
                                              .row_stride_bytes        = 128,
                                              .buffer_size_bytes       = 512,
                                              .address_alignment_bytes = 4096,
                                              .memory_access           = host_memory_access_e::read_write},
                            .max_slots     = 1,
                            .initial_slots = 1
    });
    auto                     lease = input->acquire_upload_buffer_for(3s);
    if (!lease.has_value()) {
        ADD_FAILURE() << "Expected lease to hold a value";
        return;
    }
    EXPECT_EQ(reinterpret_cast<uintptr_t>(lease->writable_host_bytes().data()) % 4096, 0);
    EXPECT_LE(uploads.memory_usage(), uploads.memory_budget());
}

TEST_F(transfer_vulkan, FullHdFramesRetainEveryActiveByteAcrossRepeatedTransfers)
{
    texture_upload_service_s   uploads(gpu(), 128 << 20);
    texture_readback_service_s downloads(gpu(), 128 << 20);
    constexpr size_t           width  = 1920;
    constexpr size_t           height = 1080;
    constexpr size_t           stride = (width * 4) + 64;
    const host_frame_layout_s  layout{
         .image_dimensions  = {width, height},
         .pixel_format      = host_pixel_format_e::rgba_u8,
         .row_stride_bytes  = stride,
         .buffer_size_bytes = stride * height
    };

    auto input =
        uploads.create_stream({.host_layout = layout, .max_slots = 2, .initial_slots = 2, .generate_mip_maps = false});
    auto readback_layout          = layout;
    readback_layout.memory_access = host_memory_access_e::read_only;
    auto output = downloads.create_stream({.host_layout = readback_layout, .max_slots = 2, .initial_slots = 2});
    ASSERT_TRUE(output->wait_for_initial_slots(3s));
    for (unsigned sequence = 0; sequence < 8; ++sequence) {
        auto lease = input->acquire_upload_buffer_for(3s);
        if (!lease.has_value()) {
            ADD_FAILURE() << "Expected lease to hold a value";
            return;
        }
        auto                   bytes = lease->writable_host_bytes();
        std::vector<std::byte> expected(stride * height);
        for (size_t y = 0; y < height; ++y) {
            for (size_t x = 0; x < width * 4; ++x) {
                expected[(y * stride) + x] =
                    std::byte{static_cast<uint8_t>((x * 13 + y * 7 + size_t{sequence} * 23) % 256)};
            }
        }

        std::ranges::copy(expected, bytes.begin());
        const auto id = lease->upload_id();
        ASSERT_TRUE(lease->submit());
        lease.reset();
        ASSERT_EQ(input->wait_for_upload(id), texture_upload_wait_result_e::ready);
        auto source = input->select_completed_upload(id);
        ASSERT_TRUE(source);
        auto target = until([&] { return output->try_acquire_render_target(); });
        if (!target.has_value()) {
            ADD_FAILURE() << "Expected target to hold a value";
            return;
        }

        auto commands = until([&] { return gpu().try_record(); });
        ASSERT_TRUE(commands);
        draw_texture(
            *commands, source->texture(), target->texture(), {}, 1, color_operation_e::none, compositing_e::replace);
        target->set_program_target_time(utils::flicks{sequence + 1});
        target->submit(commands->submit());
        target.reset();
        auto result = until([&] { return output->try_consume_oldest(); });
        if (!result.has_value()) {
            ADD_FAILURE() << "Expected result to hold a value";
            return;
        }
        EXPECT_EQ(result->program_target_time(), utils::flicks{sequence + 1});
        EXPECT_TRUE(std::ranges::equal(result->readable_host_bytes(), expected)) << "sequence " << sequence;
    }
}

TEST_F(transfer_vulkan, DirectBackendDoesNotAllocateASecondDeviceFrame)
{
    for (const auto format : {host_pixel_format_e::rgba_u8, host_pixel_format_e::v210}) {
        const host_frame_layout_s layout{
            .image_dimensions  = {1920, 1080},
            .pixel_format      = format,
            .row_stride_bytes  = format == host_pixel_format_e::v210 ? size_t{5120}
               : size_t{7680},
            .buffer_size_bytes = (format == host_pixel_format_e::v210 ? size_t{5120}
               : size_t{7680}
               ) * 1080,
        };
        const auto                             plan = gpu::transfer::detail::make_texture_transfer_plan(layout);
        texture_frame_s                        frame(gpu(), layout, sampling_e::linear);
        gpu::transfer::detail::frame_staging_s transfer(
            gpu(), plan, gpu::transfer::detail::transfer_direction_e::cpu_to_gpu, &frame);
        EXPECT_EQ(transfer.backend_name(), gpu().uses_cuda_transfers() ? "cuda-vulkan-direct" : "vulkan-staging");
        // Host allocation plus native alignment slack is allowed. A second full
        // device frame would nearly double this and violate the direct contract.
        EXPECT_LT(transfer.allocation_bytes(), layout.buffer_size_bytes + (1U << 20));
    }
}

// Every host byte layout shares one CUDA-compatible storage format. Exercise the
// shader mappings in both directions and regenerate mipmaps after slot reuse.
TEST_F(transfer_vulkan, RawChannelOrdersAndMipmapsSurviveRepeatedDirectTransfers)
{
    for (const auto format : {host_pixel_format_e::rgba_u8,
                              host_pixel_format_e::bgra_u8,
                              host_pixel_format_e::bgrx_u8,
                              host_pixel_format_e::argb_u8}) {
        SCOPED_TRACE(static_cast<int>(format));
        texture_upload_service_s   uploads(gpu(), 4 << 20);
        texture_readback_service_s downloads(gpu(), 4 << 20);
        const host_frame_layout_s  input_layout{
             .image_dimensions  = {16, 8},
             .pixel_format      = format,
             .row_stride_bytes  = 80,
             .buffer_size_bytes = 640,
        };
        auto input = uploads.create_stream(
            {.host_layout = input_layout, .max_slots = 2, .initial_slots = 2, .generate_mip_maps = true});
        auto output = downloads.create_stream({
            .host_layout   = {.image_dimensions  = {4, 2},
                              .pixel_format      = format,
                              .row_stride_bytes  = 24,
                              .buffer_size_bytes = 48,
                              .memory_access     = host_memory_access_e::read_only},
            .max_slots     = 1,
            .initial_slots = 1
        });
        ASSERT_TRUE(output->wait_for_initial_slots(3s));
        const auto order = format == host_pixel_format_e::argb_u8   ? channel_order_e::argb
                           : format == host_pixel_format_e::rgba_u8 ? channel_order_e::rgba
                                                                    : channel_order_e::bgra;
        for (uint8_t sequence = 0; sequence < 4; ++sequence) {
            auto lease = input->acquire_upload_buffer_for(3s);
            ASSERT_TRUE(lease);
            const std::array<std::byte, 4> raw{std::byte(31 + sequence),
                                               std::byte(73 + sequence),
                                               std::byte(119 + sequence),
                                               std::byte(183 + sequence)};
            auto                           host = lease->writable_host_bytes();
            std::ranges::fill(host, std::byte{241}); // Row padding must never enter the image.
            for (size_t row = 0; row < 8; ++row) {
                for (size_t pixel = 0; pixel < 16; ++pixel) {
                    std::ranges::copy(raw, host.begin() + static_cast<ptrdiff_t>(row * 80 + pixel * 4));
                }
            }
            const auto id = lease->upload_id();
            ASSERT_TRUE(lease->submit());
            ASSERT_EQ(input->wait_for_upload(id), texture_upload_wait_result_e::ready);
            auto source = input->select_completed_upload(id);
            ASSERT_TRUE(source);
            EXPECT_EQ(source->texture()->format(), format_e::rgba_unorm8);
            EXPECT_GT(source->texture()->mip_levels(), 1);
            auto target = until([&] { return output->try_acquire_render_target(); });
            ASSERT_TRUE(target);
            auto commands = until([&] { return gpu().try_record(); });
            ASSERT_TRUE(commands);
            draw_texture(*commands,
                         source->texture(),
                         target->texture(),
                         {},
                         1,
                         color_operation_e::none,
                         compositing_e::replace,
                         order);
            target->submit(commands->submit());
            commands.reset();
            target.reset();
            auto result = until([&] { return output->try_consume_oldest(); });
            ASSERT_TRUE(result);
            auto expected = raw;
            if (format == host_pixel_format_e::bgrx_u8) {
                expected[3] = std::byte{255};
            }
            const auto bytes = result->readable_host_bytes();
            for (size_t row = 0; row < 2; ++row) {
                for (size_t pixel = 0; pixel < 4; ++pixel) {
                    for (size_t channel = 0; channel < 4; ++channel) {
                        EXPECT_EQ(bytes[row * 24 + pixel * 4 + channel], expected[channel]);
                    }
                }
            }
        }
    }
}

TEST_F(transfer_vulkan, NodeColorParametersPreserveRec709RedPackingAndDecoding)
{
    auto source     = gpu().create_texture({.width = 6, .height = 1});
    auto decoded    = gpu().create_texture({.width = 6, .height = 1});
    auto packed     = gpu().create_buffer(16, host_access_e::readback);
    auto downloaded = gpu().create_buffer(48, host_access_e::readback);

    auto commands = gpu().try_record();
    ASSERT_TRUE(commands);
    commands->clear(source, {1, 0, 0, 1});
    commands->pack_v210(source,
                        packed,
                        color_parameters(get_color_transfer_to_yuv(color_transfer_e::Rec709),
                                         get_gamut_transfer_from_rec709(color_transfer_e::Rec709),
                                         color_conversion_direction_e::to_yuv),
                        16);
    commands->unpack_v210(packed,
                          decoded,
                          color_parameters(get_color_transfer_from_yuv(color_transfer_e::Rec709),
                                           get_gamut_transfer_to_rec709(color_transfer_e::Rec709),
                                           color_conversion_direction_e::from_yuv),
                          16);
    commands->readback(decoded, downloaded);
    EXPECT_EQ(commands->submit().wait(3s), wait_result_e::ready);
    uint32_t word{};
    std::memcpy(&word, packed.readable_bytes().data(), 4);
    // Independent legal-range Rec.709 red: Y=64+876*.2126,
    // Cb=512-448*.2126/(1-.0722), Cr=960.
    EXPECT_NEAR((word >> 10) & 1023, 250, 1);
    EXPECT_NEAR(word & 1023, 409, 1);
    EXPECT_EQ((word >> 20) & 1023, 960);
    std::array<uint16_t, 24> pixels{};
    std::memcpy(pixels.data(), downloaded.readable_bytes().data(), 48);
    for (size_t pixel = 0; pixel < 6; ++pixel) {
        EXPECT_NEAR(pixels.at(pixel * 4), 65535, 256);
        EXPECT_LT(pixels.at((pixel * 4) + 1), 128);
        EXPECT_LT(pixels.at((pixel * 4) + 2), 128);
        EXPECT_EQ(pixels.at((pixel * 4) + 3), 65535);
    }
}
} // namespace

#ifdef MIXIMUS_TEST_DECKLINK
TEST_F(transfer_vulkan, DeckLinkUsesTransferMemoryDirectlyAndRetainsItThroughSdkReferences)
{
    using namespace decklink_sdk;
    using namespace nodes::decklink::detail;
    texture_upload_service_s   uploads(gpu(), 1 << 20);
    texture_readback_service_s downloads(gpu(), 1 << 20);
    const host_frame_layout_s  layout{
         .image_dimensions        = {48, 2},
         .pixel_format            = host_pixel_format_e::v210,
         .row_stride_bytes        = 256,
         .buffer_size_bytes       = 512,
         .address_alignment_bytes = 4096
    };

    auto input =
        uploads.create_stream({.host_layout = layout, .max_slots = 2, .initial_slots = 2, .generate_mip_maps = false});
    // Wait for both asynchronous allocations before simulating immediate reuse;
    // steady-state SDK access intentionally cannot wait for a missing slot.
    auto warm_first  = input->acquire_upload_buffer_for(3s);
    auto warm_second = input->acquire_upload_buffer_for(3s);
    ASSERT_TRUE(warm_first);
    ASSERT_TRUE(warm_second);
    warm_first.reset();
    warm_second.reset();
    auto                               allocator = make_decklink_ptr<input_video_buffer_allocator_s>(512, input);
    decklink_ptr<IDeckLinkVideoBuffer> capture;
    ASSERT_EQ(allocator->AllocateVideoBuffer(capture.releaseAndGetAddressOf()), S_OK);
    auto custom = query_input_video_buffer(capture.get());
    ASSERT_TRUE(custom);
    ASSERT_EQ(capture->StartAccess(bmdBufferAccessWrite), S_OK);
    void* capture_address{};
    ASSERT_EQ(capture->GetBytes(&capture_address), S_OK);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(capture_address) % 4096, 0);
    std::memset(capture_address, 77, 512); // Simulated DeckLink DMA writes its GetBytes address.
    ASSERT_EQ(capture->EndAccess(bmdBufferAccessWrite), S_OK);
    auto first = custom->take_upload();
    if (!first.has_value()) {
        ADD_FAILURE() << "Expected first to hold a value";
        return;
    }
    EXPECT_EQ(first->writable_host_bytes().data(), capture_address);
    const auto first_id = first->upload_id();

    // The SDK reuses the same COM buffer, but the prior frame's lease still owns
    // its address. A new capture must not overwrite that registered allocation.
    ASSERT_EQ(capture->StartAccess(bmdBufferAccessWrite), S_OK);
    void* next_address{};
    ASSERT_EQ(capture->GetBytes(&next_address), S_OK);
    EXPECT_NE(next_address, capture_address);
    std::memset(next_address, 119, 512);
    ASSERT_EQ(capture->EndAccess(bmdBufferAccessWrite), S_OK);
    auto second = custom->take_upload();
    if (!second.has_value()) {
        ADD_FAILURE() << "Expected second to hold a value";
        return;
    }
    EXPECT_EQ(second->writable_host_bytes().data(), next_address);
    EXPECT_FALSE(input->try_acquire_upload_buffer());
    custom  = nullptr;
    capture = nullptr;
    allocator->shutdown_and_wait(); // GPU leases outlive the SDK allocator pool.
    allocator = nullptr;

    ASSERT_TRUE(first->submit());
    ASSERT_EQ(input->wait_for_upload(first_id), texture_upload_wait_result_e::ready);
    auto selected = input->select_completed_upload(first_id);
    ASSERT_TRUE(selected);
    EXPECT_FALSE(input->try_acquire_upload_buffer());
    first.reset();
    const auto second_id = second->upload_id();
    ASSERT_TRUE(second->submit());
    second.reset();
    ASSERT_EQ(input->wait_for_upload(second_id), texture_upload_wait_result_e::ready);
    auto replacement = input->select_completed_upload(second_id);
    ASSERT_TRUE(replacement);
    EXPECT_FALSE(input->try_acquire_upload_buffer()); // Selected GPU frame still retained.

    auto readback_layout          = layout;
    readback_layout.memory_access = host_memory_access_e::read_write; // SDK may request write access.
    auto output = downloads.create_stream({.host_layout = readback_layout, .max_slots = 1, .initial_slots = 1});
    ASSERT_TRUE(output->wait_for_initial_slots(3s));
    auto target = output->try_acquire_render_target();
    if (!target.has_value()) {
        ADD_FAILURE() << "Expected target to hold a value";
        return;
    }

    auto commands = until([&] { return gpu().try_record(); });
    ASSERT_TRUE(commands);
    commands->copy(selected->buffer(), target->buffer(), 512);
    target->submit(commands->submit());
    commands.reset();
    target.reset();
    auto frame = until([&] { return output->try_consume_oldest(); });
    if (!frame.has_value()) {
        ADD_FAILURE() << "Expected frame to hold a value";
        return;
    }
    const auto host_address = frame->readable_host_bytes().data();
    auto       playback     = make_decklink_ptr<output_video_buffer_s>(std::move(*frame));
    frame.reset();
    ASSERT_EQ(playback->StartAccess(bmdBufferAccessWrite), S_OK);
    void* playback_address{};
    ASSERT_EQ(playback->GetBytes(&playback_address), S_OK);
    EXPECT_EQ(playback_address, host_address); // No intermediate SDK CPU buffer.
    EXPECT_EQ(reinterpret_cast<uintptr_t>(playback_address) % 4096, 0);
    uint64_t size{};
    ASSERT_EQ(playback->GetSize(&size), S_OK);
    EXPECT_EQ(size, 512);
    EXPECT_TRUE(std::all_of(host_address, host_address + size, [](std::byte b) { return b == std::byte{77}; }));
    static_cast<std::byte*>(playback_address)[0] = std::byte{42};
    ASSERT_EQ(playback->EndAccess(bmdBufferAccessWrite), S_OK);
    auto sdk_reference = playback;
    playback           = nullptr;
    EXPECT_FALSE(output->try_acquire_render_target());
    ASSERT_EQ(sdk_reference->GetBytes(&playback_address), S_OK);
    EXPECT_EQ(static_cast<std::byte*>(playback_address)[0], std::byte{42});
    sdk_reference = nullptr;
    EXPECT_TRUE(until([&] { return output->try_acquire_render_target(); }));
    selected.reset();
    EXPECT_TRUE(until([&] { return input->try_acquire_upload_buffer(); }));
}

#endif

int main(int argc, char** argv)
{
    bool disable_cuda{};
    bool log_debug{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--disable-cuda" || argument == "--log-debug") {
            if (argument == "--disable-cuda") {
                disable_cuda = true;
            } else {
                log_debug = true;
            }

            for (int j = i; j + 1 < argc; ++j) {
                argv[j] = argv[j + 1];
            }

            argv[--argc] = nullptr;
            --i;
        }
    }

    transfer_vulkan::configure(disable_cuda, log_debug);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
