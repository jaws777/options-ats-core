#include <gtest/gtest.h>
#include "infra/seqlock.hpp"

#include <thread>

using namespace options_ats_core::infra;

struct BboSnap {
    int64_t bid{0};
    int64_t ask{0};
    int32_t bid_size{0};
    int32_t ask_size{0};
};

TEST(SeqlockTest, BasicStoreAndLoad) {
    Seqlock<BboSnap> sl;
    sl.store(BboSnap{.bid = 1000, .ask = 1005, .bid_size = 100, .ask_size = 200});
    auto loaded = sl.load();
    EXPECT_EQ(loaded.bid, 1000);
    EXPECT_EQ(loaded.ask, 1005);
}

TEST(SeqlockTest, SequenceIncrementsByTwo) {
    Seqlock<BboSnap> sl;
    EXPECT_EQ(sl.sequence(), 0u);
    sl.store(BboSnap{});
    EXPECT_EQ(sl.sequence(), 2u);
}

TEST(SeqlockTest, ConcurrentWriterReader) {
    Seqlock<BboSnap> sl;
    constexpr int kWrites = 100'000;
    std::atomic<bool> done{false};

    std::thread writer([&] {
        for (int i = 1; i <= kWrites; ++i) {
            sl.store(BboSnap{.bid = i, .ask = i + 5, .bid_size = i, .ask_size = i});
        }
        done.store(true, std::memory_order_release);
    });

    int consistent = 0;
    while (!done.load(std::memory_order_acquire) || consistent < 100) {
        auto snap = sl.load();
        if (snap.bid == 0) continue; // Initial state.
        EXPECT_EQ(snap.ask, snap.bid + 5) << "Inconsistent read at bid=" << snap.bid;
        ++consistent;
        if (consistent > 10'000'000) break;
    }

    writer.join();
    EXPECT_GT(consistent, 0);
}
