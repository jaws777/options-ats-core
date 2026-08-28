#pragma once
/// aeron_transport.hpp — Aeron-compatible IPC transport abstraction.
///
/// Aeron (https://github.com/real-logic/aeron) is a high-performance,
/// low-latency messaging system built on shared memory IPC and UDP
/// multicast. This file provides a thin design reference that shows
/// how the SPSC queue and Disruptor patterns in this library map to
/// Aeron's API; it does not depend on the Aeron C++ library.
///
/// Key Aeron concepts relevant to an options ATS architecture:
///
///   Publication/Subscription model:
///   - A Publication writes messages into a log buffer (term buffer).
///   - A Subscription reads from the same log buffer via shared memory.
///   - Zero-copy: messages are written directly into the term buffer.
///   - No broker: direct memory-mapped IPC between producer and consumer.
///
///   Channels and Streams:
///   - Channel: transport binding (e.g., "aeron:ipc" for local IPC,
///     "aeron:udp?endpoint=host:port" for network).
///   - Stream ID: multiplexes logical message flows within a channel.
///   - Typical pattern: each MM connection has its own stream ID.
///
///   Back-pressure:
///   - Publication::offer() returns BACK_PRESSURED if the subscriber
///     can't keep up. Publisher must handle this (drop, retry, or slow).
///   - This maps directly to the fan-out design used here: a slow
///     subscriber gets back-pressured without affecting other subscribers.
///
///   Reliability:
///   - Aeron supports reliable delivery with retransmissions (UDP)
///     or lossless IPC (shared memory).
///   - For the ATS matching hot path: "aeron:ipc" — zero network hop.
///   - For MM connectivity: "aeron:udp" with reliability.
///
///   Fragmentation:
///   - Messages larger than the MTU are automatically fragmented
///     and reassembled by Aeron.
///
/// In production, SpscQueue/LmaxDisruptor can be replaced with:
///   aeron::Publication::offer()  — for publishing
///   aeron::Subscription::poll()  — for consuming
///
/// Design note: the SPSC queue and Disruptor in this library use the same
/// underlying pattern as Aeron — a pre-allocated ring buffer with
/// sequence-based coordination. Aeron layers channel management,
/// fragmentation, flow control, and multi-subscriber support on top.

#include <cstdint>

namespace options_ats_core::infra {

/// Aeron return codes from Publication::offer().
/// These map directly to back-pressure handling strategies.
enum class AeronOfferResult : int32_t {
    Success          = 0,     // Message accepted.
    BackPressured    = -1,    // Subscriber can't keep up. Retry or drop.
    NotConnected     = -2,    // No subscriber connected.
    AdminAction      = -3,    // Internal Aeron housekeeping. Retry.
    MaxPositionExceeded = -4, // Term buffer full. Rotate.
};

/// Maps internal message types to Aeron stream IDs.
/// Each logical message flow gets its own stream for isolation.
///
/// In Aeron, you'd create a Publication per stream:
///   aeron->addPublication("aeron:ipc", kStreamRetailOrders);
///   aeron->addPublication("aeron:ipc", kStreamMmQuotes);
///
/// Subscribers subscribe to specific streams:
///   aeron->addSubscription("aeron:ipc", kStreamRetailOrders, handler);
namespace stream_ids {
    static constexpr int32_t kRetailOrders    = 1;  // Client gateway → OMS.
    static constexpr int32_t kMmQuotes        = 2;  // MM gateway → Quote Manager.
    static constexpr int32_t kMatchResults    = 3;  // Matching engine → fill distribution.
    static constexpr int32_t kMarketDataOut   = 4;  // Book updates → MM subscribers.
    static constexpr int32_t kAuditEvents     = 5;  // All components → WAL writer.
    static constexpr int32_t kRiskUpdates     = 6;  // Matching engine → risk engine.
    static constexpr int32_t kNbboUpdates     = 7;  // OPRA handler → NBBO table.
    static constexpr int32_t kControlPlane    = 8;  // Config reload, admin commands.
} // namespace stream_ids

/// Message header for Aeron-transported messages.
/// Every message on every stream starts with this header.
/// Aeron itself adds its own framing — this is the application header.
///
/// Design choice: fixed-size header (32 bytes) for alignment and
/// predictable parsing. The body follows immediately after.
struct alignas(32) AeronMsgHeader {
    uint64_t sequence;         // Monotonic per-stream sequence number.
    uint64_t timestamp_ns;     // Nanosecond timestamp (source clock).
    int32_t stream_id;         // Logical stream (from stream_ids).
    int32_t body_length;       // Bytes following this header.
    uint32_t msg_type;         // Application-level message type.
    uint32_t reserved;         // Alignment padding / future use.
};

static_assert(sizeof(AeronMsgHeader) == 32, "Header must be exactly 32 bytes");

/// How the library's components map to Aeron channels in an ATS architecture:
///
///   +------------------+     aeron:ipc        +------------------+
///   | Client Gateway   | ---- stream 1 -----> |       OMS        |
///   | (FIX parser)     |                      | (state machine)  |
///   +------------------+                      +--------+---------+
///                                                      |
///   +------------------+     aeron:ipc                 v
///   | MM Gateway       | ---- stream 2 -----> +------------------+
///   | (binary parser)  |                      | Matching Engine  |
///   +------------------+                      | (series_book)    |
///                                             +--------+---------+
///                                                      |
///   +------------------+     aeron:ipc          stream 3/4
///   | OPRA Handler     | ---- stream 7 -----> +------------------+
///   |                  |                      | NBBO Table       |
///   +------------------+                      | (seqlock)        |
///                                             +------------------+
///
///   All components -------- stream 5 -------> WAL Writer
///   Control plane --------- stream 8 -------> All (config reload)
///
/// Key advantage of Aeron over raw SPSC:
///   - Built-in flow control and back-pressure signaling
///   - Multi-subscriber support (market data fan-out)
///   - Transparent IPC-to-network bridging (same API for local
///     and remote, just change the channel URI)
///   - Counters and monitoring built in (message rates, back-pressure events)
///   - Archive module for replay (maps to the WAL requirement here)

} // namespace options_ats_core::infra
