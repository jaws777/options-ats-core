#pragma once
/// wal.hpp — Write-ahead log for deterministic replay and audit trail.
///
/// Every inbound event (order, quote update, NBBO update) gets a monotonic
/// sequence number and nanosecond timestamp, then is appended to the WAL.
/// The same event sequence can support:
///   1. Internal event reconstruction
///   2. Deterministic replay (debugging, failover)
///   3. Active-passive replication (the passive replays the WAL)
///
/// This is an in-memory WAL for demonstration and testing. A production
/// deployment would mmap a file or write to a ring buffer backed by
/// persistent storage.
///
/// This demonstration is not a CAT reporting system or regulatory record.

#include "core/expected.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace options_ats_core::infra {

enum class WalError : uint8_t {
    SequenceGap,
    BufferFull,
};

[[nodiscard]] constexpr std::string_view to_string(WalError e) noexcept {
    switch (e) {
        case WalError::SequenceGap: return "SequenceGap";
        case WalError::BufferFull:  return "BufferFull";
    }
    return "Unknown";
}

enum class EventType : uint8_t {
    NewOrder,
    CancelRequest,
    Fill,
    QuoteUpdate,
    NbboUpdate,
    RiskReject,
    SystemEvent,
};

[[nodiscard]] constexpr std::string_view to_string(EventType t) noexcept {
    switch (t) {
        case EventType::NewOrder:      return "NewOrder";
        case EventType::CancelRequest: return "CancelRequest";
        case EventType::Fill:          return "Fill";
        case EventType::QuoteUpdate:   return "QuoteUpdate";
        case EventType::NbboUpdate:    return "NbboUpdate";
        case EventType::RiskReject:    return "RiskReject";
        case EventType::SystemEvent:   return "SystemEvent";
    }
    return "Unknown";
}

/// A single WAL entry. Fixed-size for cache-friendly sequential access.
/// Every entry is the same size, so iterating the WAL is a linear scan over
/// contiguous memory — no pointer chasing, no variable-length decoding,
/// and the hardware prefetcher can predict the access pattern.
///
/// payload_a / payload_b are generic int64_t fields to avoid union or variant
/// complexity. The EventType discriminates their meaning (e.g., for a Fill,
/// payload_a = price raw, payload_b = quantity). In production you would
/// either template WalEntry on EventType or use a discriminated union with
/// type-safe accessors — this simplified form is sufficient for demonstration.
struct WalEntry {
    uint64_t sequence{0};
    uint64_t timestamp_ns{0};
    EventType event_type{EventType::SystemEvent};
    uint64_t entity_id{0};     // Order ID, MM ID, or series ID.
    int64_t payload_a{0};      // Context-dependent (price raw, quantity, etc.).
    int64_t payload_b{0};      // Context-dependent.
};

/// In-memory write-ahead log. Single writer. Readers iterate by sequence.
class Wal {
public:
    // Pre-allocate backing storage so that append() on the hot path never
    // triggers a heap allocation. vector::reserve allocates once at
    // construction; subsequent push_back calls within capacity are O(1)
    // with no malloc. If we skipped this, push_back could trigger a
    // reallocation + memcpy mid-match, adding microseconds of jitter.
    explicit Wal(std::size_t capacity = 1'000'000) : capacity_{capacity} {
        entries_.reserve(capacity);
    }

    /// Append an entry. Returns the assigned sequence number.
    ///
    /// Monotonic sequence assignment is critical: the sequence number is the
    /// total ordering of all events in the matching engine. Replay correctness
    /// depends on every event getting a unique, gap-free, strictly increasing
    /// sequence. Two WAL instances processing identical inputs must produce
    /// identical sequence assignments — this is verified by verify_parity().
    ///
    /// next_sequence_++ is a plain (non-atomic) increment because the WAL is
    /// single-writer: only the matching thread appends. In a multi-writer
    /// design (e.g., multiple matching threads sharing a WAL), this would
    /// need to be std::atomic<uint64_t> with fetch_add(1, relaxed).
    [[nodiscard]] core::expected<uint64_t, WalError>
    append(EventType type, uint64_t timestamp_ns, uint64_t entity_id,
           int64_t payload_a = 0, int64_t payload_b = 0) {
        if (entries_.size() >= capacity_) {
            return core::unexpected(WalError::BufferFull);
        }
        uint64_t seq = next_sequence_++;
        entries_.push_back(WalEntry{
            .sequence = seq,
            .timestamp_ns = timestamp_ns,
            .event_type = type,
            .entity_id = entity_id,
            .payload_a = payload_a,
            .payload_b = payload_b,
        });
        return seq;
    }

    /// Read entry by sequence number. Returns nullptr if not found.
    [[nodiscard]] const WalEntry* read(uint64_t sequence) const noexcept {
        if (sequence >= entries_.size()) return nullptr;
        if (entries_[sequence].sequence != sequence) return nullptr;
        return &entries_[sequence];
    }

    /// Iterate entries in sequence order from start_seq.
    [[nodiscard]] std::vector<WalEntry>
    read_range(uint64_t start_seq, uint64_t count) const {
        std::vector<WalEntry> result;
        uint64_t end = std::min(start_seq + count,
                                static_cast<uint64_t>(entries_.size()));
        for (uint64_t i = start_seq; i < end; ++i) {
            result.push_back(entries_[i]);
        }
        return result;
    }

    [[nodiscard]] uint64_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] uint64_t next_sequence() const noexcept { return next_sequence_; }

    /// Verify replay parity: compare two WALs entry-by-entry.
    ///
    /// Deterministic replay verification. Given identical input sequences
    /// (market data, orders), two instances of the matching engine must
    /// produce identical WAL entries — same sequence numbers, same event
    /// types, same entity IDs, same payloads. Any divergence at sequence N
    /// means the engine is non-deterministic at that point: a bug.
    ///
    /// This is the deployment gate: before promoting a new engine binary to
    /// production, we replay the previous day's WAL through both the old and
    /// new binaries and verify parity. If they diverge, the new binary does
    /// not ship. This catches floating-point ordering differences, hash map
    /// iteration order changes, uninitialized memory reads, and race
    /// conditions that would otherwise only manifest in production.
    ///
    /// Note: timestamps are intentionally NOT compared. Two runs will have
    /// different wall-clock times; what matters is that the same inputs
    /// produce the same outputs (events, fills, rejects) in the same order.
    ///
    /// Returns: true if all entries match, or core::unexpected(N) where N is
    /// the first divergent sequence number — the debugging starting point.
    [[nodiscard]] core::expected<bool, uint64_t>
    verify_parity(const Wal& other) const {
        uint64_t check_count = std::min(
            static_cast<uint64_t>(entries_.size()),
            static_cast<uint64_t>(other.entries_.size()));
        for (uint64_t i = 0; i < check_count; ++i) {
            const auto& a = entries_[i];
            const auto& b = other.entries_[i];
            if (a.sequence != b.sequence ||
                a.event_type != b.event_type ||
                a.entity_id != b.entity_id ||
                a.payload_a != b.payload_a ||
                a.payload_b != b.payload_b) {
                return core::unexpected(i); // Divergence at this sequence.
            }
        }
        return true;
    }

private:
    std::vector<WalEntry> entries_;
    std::size_t capacity_;
    uint64_t next_sequence_{0};
};

} // namespace options_ats_core::infra
