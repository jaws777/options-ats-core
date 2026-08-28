#include "core/fixed_price.hpp"
#include "infra/lmax_disruptor.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

using options_ats_core::core::FixedPrice;
using options_ats_core::infra::LmaxDisruptor;

struct MarketUpdate {
    uint64_t sequence{0};
    uint64_t instrument_id{0};
    FixedPrice bid{};
    FixedPrice ask{};
    int32_t bid_qty{0};
    int32_t ask_qty{0};
};

constexpr std::size_t kRingCapacity = 1024;
constexpr uint64_t kUpdateCount = 8;
constexpr uint64_t kInstrumentId = 42;
constexpr int32_t kTopOfBookQty = 10;

void produce(LmaxDisruptor<MarketUpdate, kRingCapacity>& disruptor) {
    for (uint64_t seq = 0; seq < kUpdateCount; ++seq) {
        for (;;) {
            auto claim = disruptor.try_claim();
            if (!claim.has_value()) {
                continue;
            }

            claim->event->sequence = claim->sequence;
            claim->event->instrument_id = kInstrumentId;
            claim->event->bid = FixedPrice::from_dollars_cents(100 + static_cast<int64_t>(seq), 0);
            claim->event->ask = FixedPrice::from_dollars_cents(100 + static_cast<int64_t>(seq), 5);
            claim->event->bid_qty = kTopOfBookQty + static_cast<int32_t>(seq);
            claim->event->ask_qty = kTopOfBookQty + static_cast<int32_t>(seq);

            disruptor.publish(claim->sequence);
            break;
        }
    }
}

void consume(LmaxDisruptor<MarketUpdate, kRingCapacity>& disruptor) {
    uint64_t processed = 0;
    while (processed < kUpdateCount) {
        auto record = disruptor.try_consume();
        if (!record.has_value()) {
            continue;
        }

        std::array<char, FixedPrice::kCharsCapacity> bid_buf{};
        std::array<char, FixedPrice::kCharsCapacity> ask_buf{};

        std::cout
            << "seq=" << record->event->sequence
            << " instrument=" << record->event->instrument_id
            << " bid=" << record->event->bid.to_string_view(bid_buf)
            << " ask=" << record->event->ask.to_string_view(ask_buf)
            << " bid_qty=" << record->event->bid_qty
            << " ask_qty=" << record->event->ask_qty
            << '\n';

        disruptor.release(record->sequence);
        ++processed;
    }
}

} // namespace

int main() {
    LmaxDisruptor<MarketUpdate, kRingCapacity> disruptor;

    std::thread producer([&] { produce(disruptor); });
    std::thread consumer([&] { consume(disruptor); });

    producer.join();
    consumer.join();
    return 0;
}
