#include <catch2/catch_test_macros.hpp>
#include "infra/spsc_queue.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace options_ats_core::infra;

TEST_CASE("SpscQueue: basic push and pop", "[spsc]") {
    SpscQueue<int, 4> q;

    REQUIRE(q.empty());
    REQUIRE(q.size() == 0);
    REQUIRE(q.capacity() == 4);

    REQUIRE(q.try_push(42));
    REQUIRE(q.size() == 1);

    auto val = q.try_pop();
    REQUIRE(val.has_value());
    REQUIRE(*val == 42);
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue: full queue returns false on push", "[spsc]") {
    SpscQueue<int, 4> q;

    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE(q.try_push(3));
    REQUIRE(q.try_push(4));
    REQUIRE_FALSE(q.try_push(5)); // Full.

    REQUIRE(q.size() == 4);
}

TEST_CASE("SpscQueue: empty queue returns nullopt", "[spsc]") {
    SpscQueue<int, 4> q;

    auto val = q.try_pop();
    REQUIRE_FALSE(val.has_value());

    int out = -1;
    REQUIRE_FALSE(q.try_pop(out));
    REQUIRE(out == -1); // Unchanged.
}

TEST_CASE("SpscQueue: FIFO ordering", "[spsc]") {
    SpscQueue<int, 8> q;

    for (int i = 0; i < 8; ++i) {
        REQUIRE(q.try_push(i));
    }

    for (int i = 0; i < 8; ++i) {
        auto val = q.try_pop();
        REQUIRE(val.has_value());
        REQUIRE(*val == i);
    }
}

TEST_CASE("SpscQueue: wrap-around", "[spsc]") {
    SpscQueue<int, 4> q;

    // Fill and drain twice to force index wrap-around.
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 4; ++i) {
            REQUIRE(q.try_push(round * 10 + i));
        }
        for (int i = 0; i < 4; ++i) {
            auto val = q.try_pop();
            REQUIRE(val.has_value());
            REQUIRE(*val == round * 10 + i);
        }
        REQUIRE(q.empty());
    }
}

TEST_CASE("SpscQueue: move semantics", "[spsc]") {
    SpscQueue<std::vector<int>, 4> q;

    std::vector<int> v = {1, 2, 3};
    REQUIRE(q.try_push(std::move(v)));
    REQUIRE(v.empty()); // Moved from.

    auto result = q.try_pop();
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 3);
    REQUIRE((*result)[2] == 3);
}

TEST_CASE("SpscQueue: concurrent producer-consumer", "[spsc]") {
    constexpr std::size_t kCount = 100'000;
    SpscQueue<uint64_t, 1024> q;

    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount; ++i) {
            while (!q.try_push(i)) {
                // Spin — producer waits for space.
            }
        }
    });

    std::vector<uint64_t> received;
    received.reserve(kCount);

    std::thread consumer([&] {
        while (received.size() < kCount) {
            uint64_t val;
            if (q.try_pop(val)) {
                received.push_back(val);
            }
        }
    });

    producer.join();
    consumer.join();

    // Verify all items received in order.
    REQUIRE(received.size() == kCount);
    for (uint64_t i = 0; i < kCount; ++i) {
        REQUIRE(received[i] == i);
    }
}

TEST_CASE("SpscQueue: pop into output parameter", "[spsc]") {
    SpscQueue<int, 4> q;

    REQUIRE(q.try_push(99));
    int out = 0;
    REQUIRE(q.try_pop(out));
    REQUIRE(out == 99);
}

namespace {

struct InstrumentedValue {
    inline static std::atomic<int> direct_constructions{0};
    inline static std::atomic<int> copies{0};
    inline static std::atomic<int> moves{0};

    InstrumentedValue() noexcept = default;

    explicit InstrumentedValue(int value_in, int marker_in) noexcept
        : value{value_in}, marker{marker_in} {
        ++direct_constructions;
    }

    InstrumentedValue(const InstrumentedValue& other) noexcept
        : value{other.value}, marker{other.marker} {
        ++copies;
    }

    InstrumentedValue& operator=(const InstrumentedValue& other) noexcept {
        value = other.value;
        marker = other.marker;
        ++copies;
        return *this;
    }

    InstrumentedValue(InstrumentedValue&& other) noexcept
        : value{other.value}, marker{other.marker} {
        ++moves;
    }

    InstrumentedValue& operator=(InstrumentedValue&& other) noexcept {
        value = other.value;
        marker = other.marker;
        ++moves;
        return *this;
    }

    int value{0};
    int marker{0};
};

} // namespace

TEST_CASE("SpscQueue: emplace avoids pre-built temporary objects", "[spsc]") {
    InstrumentedValue::direct_constructions = 0;
    InstrumentedValue::copies = 0;
    InstrumentedValue::moves = 0;

    SpscQueue<InstrumentedValue, 4> q;
    REQUIRE(q.try_emplace(7, 11));

    auto item = q.try_pop();
    REQUIRE(item.has_value());
    REQUIRE(item->value == 7);
    REQUIRE(item->marker == 11);
    REQUIRE(InstrumentedValue::direct_constructions.load() == 1);
    REQUIRE(InstrumentedValue::copies.load() == 0);
}
