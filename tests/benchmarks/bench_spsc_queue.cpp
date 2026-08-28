#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "infra/spsc_queue.hpp"

#include <thread>

using namespace options_ats_core::infra;

TEST_CASE("SpscQueue: throughput benchmark", "[!benchmark][spsc]") {
    constexpr std::size_t kOps = 1'000'000;

    BENCHMARK("push-pop 1M items (single thread)") {
        SpscQueue<uint64_t, 1024> q;
        for (uint64_t i = 0; i < kOps; ++i) {
            while (!q.try_push(i)) {
                auto discard = q.try_pop();
                (void)discard;
            }
        }
        return q.size();
    };

    BENCHMARK("producer-consumer 1M items (two threads)") {
        SpscQueue<uint64_t, 1024> q;
        uint64_t last_received = 0;

        std::thread producer([&] {
            for (uint64_t i = 0; i < kOps; ++i) {
                while (!q.try_push(i)) {}
            }
        });

        std::thread consumer([&] {
            uint64_t count = 0;
            while (count < kOps) {
                uint64_t val;
                if (q.try_pop(val)) {
                    last_received = val;
                    ++count;
                }
            }
        });

        producer.join();
        consumer.join();
        return last_received;
    };
}
