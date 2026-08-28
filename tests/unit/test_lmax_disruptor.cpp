#include <catch2/catch_test_macros.hpp>
#include "infra/lmax_disruptor.hpp"

#include <thread>
#include <vector>

using namespace options_ats_core::infra;

namespace {

struct TestEvent {
    uint64_t sequence{0};
    int value{0};
};

} // namespace

TEST_CASE("LmaxDisruptor: claim publish consume release", "[disruptor]") {
    LmaxDisruptor<TestEvent, 8> disruptor;

    auto claim = disruptor.try_claim();
    REQUIRE(claim.has_value());
    claim->event->sequence = claim->sequence;
    claim->event->value = 7;
    disruptor.publish(claim->sequence);

    auto record = disruptor.try_consume();
    REQUIRE(record.has_value());
    REQUIRE(record->sequence == 0);
    REQUIRE(record->event->sequence == 0);
    REQUIRE(record->event->value == 7);

    disruptor.release(record->sequence);
    REQUIRE(disruptor.size() == 0);
}

TEST_CASE("LmaxDisruptor: ring full until consumer advances", "[disruptor]") {
    LmaxDisruptor<TestEvent, 4> disruptor;

    for (int i = 0; i < 4; ++i) {
        auto claim = disruptor.try_claim();
        REQUIRE(claim.has_value());
        claim->event->value = i;
        disruptor.publish(claim->sequence);
    }

    auto full_claim = disruptor.try_claim();
    REQUIRE_FALSE(full_claim.has_value());
    REQUIRE(full_claim.error() == DisruptorError::RingFull);

    auto record = disruptor.try_consume();
    REQUIRE(record.has_value());
    REQUIRE(record->event->value == 0);
    disruptor.release(record->sequence);

    auto next_claim = disruptor.try_claim();
    REQUIRE(next_claim.has_value());
}

TEST_CASE("LmaxDisruptor: concurrent producer consumer preserves order", "[disruptor]") {
    constexpr uint64_t kCount = 100'000;
    LmaxDisruptor<TestEvent, 1024> disruptor;

    std::vector<uint64_t> consumed;
    consumed.reserve(kCount);

    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount; ++i) {
            for (;;) {
                auto claim = disruptor.try_claim();
                if (!claim.has_value()) {
                    continue;
                }

                claim->event->sequence = claim->sequence;
                claim->event->value = static_cast<int>(i);
                disruptor.publish(claim->sequence);
                break;
            }
        }
    });

    std::thread consumer([&] {
        while (consumed.size() < kCount) {
            auto record = disruptor.try_consume();
            if (!record.has_value()) {
                continue;
            }

            consumed.push_back(record->event->sequence);
            disruptor.release(record->sequence);
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(consumed.size() == kCount);
    for (uint64_t i = 0; i < kCount; ++i) {
        REQUIRE(consumed[static_cast<std::size_t>(i)] == i);
    }
}
