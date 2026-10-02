#include "nodes/source_name.hpp"

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using miximus::nodes::source_name_s;

TEST(SourceName, OwnedCopiesHaveIndependentStorage)
{
    const std::string   text(32, 'x');
    const source_name_s value(text);
    const auto          copy = source_name_s(value);
    EXPECT_EQ(copy.view(), text);
    EXPECT_NE(copy.view().data(), value.view().data());
}

TEST(SourceName, LongNamesRemainOwnedAcrossCopiesAndMoves)
{
    const std::string text = "Kamera — entré " + std::string(128, 'x');
    auto              copy = [&text] {
        const source_name_s value(text);
        return source_name_s(value);
    }();
    EXPECT_EQ(copy.view(), text);
    EXPECT_NE(copy.view().data(), text.data());
    const auto moved = std::move(copy);
    EXPECT_EQ(moved.view(), text);
}

TEST(SourceName, ConstructionFromSubstringOwnsItsBytes)
{
    const auto substring = [] {
        const source_name_s value(std::string(128, 'x') + "tail");
        return source_name_s(value.view().substr(128));
    }();
    EXPECT_EQ(substring.view(), "tail");
    const source_name_s empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.size(), 0);
    EXPECT_EQ(empty.view(), std::string_view{});
}

TEST(SourceName, ViewsPreserveEmbeddedNullsAndBounds)
{
    const std::string_view text("a\0b", 3);
    const source_name_s    value(text);
    EXPECT_EQ(value.view(), text);
    EXPECT_EQ(value.view().substr(1), text.substr(1));
    EXPECT_THROW((void)value.view().substr(4), std::out_of_range);
}

TEST(SourceName, BorrowedCopiesShareBytesWhileNewNamesOwnTheirBytes)
{
    for (const auto size : {size_t{12}, size_t{128}}) {
        const source_name_s owner(std::string(size, 'x'));
        const auto          borrowed = owner.borrow();
        const auto          copy     = source_name_s(borrowed);
        EXPECT_TRUE(borrowed.is_borrowed());
        EXPECT_TRUE(copy.is_borrowed());
        EXPECT_EQ(borrowed.view().data(), owner.view().data());
        EXPECT_EQ(copy.view().data(), owner.view().data());
        const source_name_s renamed(copy.view().substr(2, 5));
        EXPECT_FALSE(renamed.is_borrowed());
        EXPECT_EQ(renamed.view(), "xxxxx");
        EXPECT_EQ(owner.size(), size);
        EXPECT_EQ(borrowed.view(), owner.view());
    }
}

TEST(SourceName, BorrowingSkipsTemporaryIntermediateNames)
{
    const source_name_s owner(std::string(128, 'x'));
    auto                borrowed = [&owner] {
        const auto intermediate = owner.borrow();
        return intermediate.borrow();
    }();
    EXPECT_TRUE(borrowed.is_borrowed());
    EXPECT_EQ(borrowed.view().data(), owner.view().data());
    EXPECT_EQ(borrowed.view(), owner.view());
    const auto moved = std::move(borrowed);
    EXPECT_TRUE(moved.is_borrowed());
    EXPECT_EQ(moved.view().data(), owner.view().data());
}

TEST(SourceName, OwnedSnapshotOutlivesBorrowedOwner)
{
    const auto snapshot = [] {
        const source_name_s owner(std::string(128, 'x'));
        const auto          borrowed = owner.borrow();
        return borrowed.owned_copy();
    }();
    EXPECT_FALSE(snapshot.is_borrowed());
    EXPECT_EQ(snapshot.view(), std::string(128, 'x'));
}

TEST(SourceName, CopiesAndAssignmentsKeepTheOriginalOwner)
{
    const source_name_s owner(std::string(128, 'x'));
    auto                intermediate = owner.borrow();
    const auto          copied       = intermediate;
    const auto          reborrowed   = intermediate.borrow();
    source_name_s       assigned;
    assigned = intermediate;
    source_name_s moved;
    moved = std::move(intermediate);
    for (const auto* name : {&copied, &reborrowed, &std::as_const(assigned), &std::as_const(moved)}) {
        EXPECT_TRUE(name->is_borrowed());
        EXPECT_EQ(name->view().data(), owner.view().data());
        EXPECT_EQ(name->view(), owner.view());
    }
}

} // namespace
