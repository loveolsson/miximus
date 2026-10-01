#include "native_handle_test_support.hpp"

#include <gtest/gtest.h>
#include <system_error>
#include <utility>

namespace miximus::gpu::detail { namespace {

using namespace test;

TEST(native_handle, DuplicateSurvivesOriginalCloseAndIsNotInherited)
{
    auto original  = make_handle();
    auto duplicate = native_handle_s::duplicate(original.get());

    EXPECT_NE(original.get(), duplicate.get());

    original.reset();
    EXPECT_TRUE(valid(duplicate.get()));
    EXPECT_TRUE(not_inherited(duplicate.get()));
}

TEST(native_handle, MoveAndReleaseTransferOwnership)
{
    auto            original = make_handle();
    const auto      value    = original.get();
    native_handle_s moved(std::move(original));
    // Verify the wrapper's guaranteed empty state after moving ownership.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(original.get(), native_handle_s::invalid);
    EXPECT_EQ(moved.get(), value);

    auto       destination = make_handle();
    const auto replaced    = destination.get();
    destination            = std::move(moved);
    EXPECT_FALSE(valid(replaced));
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(moved.get(), native_handle_s::invalid);

    native_handle_s released(destination.release());
    EXPECT_EQ(destination.get(), native_handle_s::invalid);
    EXPECT_TRUE(valid(released.get()));

    released.reset();
    EXPECT_FALSE(valid(value));
}

TEST(native_handle, InvalidDuplicateThrows)
{
    EXPECT_THROW(native_handle_s::duplicate(native_handle_s::invalid), std::system_error);
}

}} // namespace miximus::gpu::detail
