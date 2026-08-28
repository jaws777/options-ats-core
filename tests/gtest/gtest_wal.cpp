#include <gtest/gtest.h>
#include "infra/wal.hpp"

using namespace options_ats_core::infra;

TEST(WalTest, AppendAndRead) {
    Wal wal;
    auto seq = wal.append(EventType::NewOrder, 1000, 42, 100, 200);
    ASSERT_TRUE(seq.has_value());
    EXPECT_EQ(*seq, 0u);

    auto entry = wal.read(0);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->event_type, EventType::NewOrder);
    EXPECT_EQ(entry->entity_id, 42u);
}

TEST(WalTest, MonotonicSequence) {
    Wal wal;
    for (int i = 0; i < 100; ++i) {
        auto seq = wal.append(EventType::Fill, 0, static_cast<uint64_t>(i));
        ASSERT_TRUE(seq.has_value());
        EXPECT_EQ(*seq, static_cast<uint64_t>(i));
    }
    EXPECT_EQ(wal.size(), 100u);
}

TEST(WalTest, BoundedCapacityRejectsAppend) {
    Wal wal(1);
    ASSERT_TRUE(wal.append(EventType::NewOrder, 0, 1).has_value());

    auto overflow = wal.append(EventType::Fill, 0, 1);
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error(), WalError::BufferFull);
    EXPECT_EQ(wal.size(), 1u);
    EXPECT_EQ(wal.next_sequence(), 1u);
}

TEST(WalTest, VerifyParityIdentical) {
    Wal a, b;
    for (int i = 0; i < 50; ++i) {
        auto seq_a = a.append(EventType::Fill, 1000, static_cast<uint64_t>(i), i * 10);
        auto seq_b = b.append(EventType::Fill, 1000, static_cast<uint64_t>(i), i * 10);
        ASSERT_TRUE(seq_a.has_value());
        ASSERT_TRUE(seq_b.has_value());
    }
    auto result = a.verify_parity(b);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result);
}

TEST(WalTest, VerifyParityDetectsDivergence) {
    Wal a, b;
    for (int i = 0; i < 10; ++i) {
        auto seq_a = a.append(EventType::Fill, 1000, static_cast<uint64_t>(i), i * 10);
        auto seq_b = b.append(EventType::Fill, 1000, static_cast<uint64_t>(i),
                              (i == 5) ? 999 : i * 10);
        ASSERT_TRUE(seq_a.has_value());
        ASSERT_TRUE(seq_b.has_value());
    }
    auto result = a.verify_parity(b);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 5u);
}

TEST(WalTest, ReadRange) {
    Wal wal;
    for (int i = 0; i < 10; ++i) {
        auto seq = wal.append(EventType::QuoteUpdate, 0, static_cast<uint64_t>(i));
        ASSERT_TRUE(seq.has_value());
    }
    auto range = wal.read_range(3, 4);
    ASSERT_EQ(range.size(), 4u);
    EXPECT_EQ(range[0].entity_id, 3u);
    EXPECT_EQ(range[3].entity_id, 6u);
}
