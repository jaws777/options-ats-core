# options-ats-core Implementation Guide

This guide walks through the public methods, data structures, and operating
purpose of `options-ats-core`. It is written for engineers reviewing or
extending the implementation, with emphasis on defensible C++20 practices:
explicit ownership, checked arithmetic, value-semantic domain types,
`core::expected` error paths, no hidden hot-path allocation where avoidable,
and deterministic behavior that can be verified by tests and replay.

This repository is infrastructure for listed-options ATS components. It is not
trading advice and does not implement a complete broker-dealer, exchange, or
regulatory reporting system.

## Design Standards Used

The implementation is reviewed against the repo conventions in `.clang-format`,
`.clang-tidy`, `CONTRIBUTING.md`, and the C++ engineering guidance reflected in
Herb Sutter's *Exceptional C++* and *More Exceptional C++*:

- Prefer simple value types and RAII over manual ownership.
- Keep invariants local, explicit, and testable.
- Use standard containers and algorithms where they fit the latency model.
- Make error handling visible at call sites with `core::expected` instead of
  silent default behavior.
- Avoid undefined behavior: checked arithmetic is used where overflow matters.
- Avoid floating point for prices and financial comparisons.
- Keep concurrency primitives narrow in scope, documented, and verified.
- Do not optimize by adding cleverness unless the data shape justifies it.

## Repository Map

| Area | Source | Purpose |
| --- | --- | --- |
| Core | `src/core/` | Fixed-point price, checked numeric helpers, domain option types, `core::expected` compatibility |
| Infrastructure | `src/infra/` | SPSC queue, LMAX-style ring, seqlock, WAL, rate limiter, Aeron mapping notes |
| Market data | `src/market_data/` | OPRA parsing, quote book, simple limit order book |
| OMS | `src/oms/` | FIX parsing and order/spread lifecycle state machines |
| Risk | `src/risk/` | Pre-trade risk and execution-quality scoring |
| Tests | `tests/` | Catch2, GoogleTest, integration, and benchmarks |
| Examples | `examples/` | Minimal OPRA consumer and disruptor usage examples |

## Core Methods

### `core/expected.hpp`

Purpose: provide the subset of C++23 `std::expected` used by this codebase when
AppleClang lacks `<expected>`.

Public surface:

| Method | Purpose |
| --- | --- |
| `std::unexpected<E>::error()` | Access the stored error value. |
| `std::expected<T, E>::has_value()` | Test success before reading the value. |
| `std::expected<T, E>::operator bool()` | Boolean success check. |
| `std::expected<T, E>::value()` | Access value with an assertion guard. |
| `std::expected<T, E>::operator*()` / `operator->()` | Lightweight value access after checking success. |
| `std::expected<T, E>::error()` | Access error with an assertion guard. |

Implementation notes:

- This is intentionally minimal. If the active compiler provides C++23
  `<expected>`, the standard implementation is used instead.
- Callers should check `has_value()` before dereferencing. Tests now avoid
  ignoring `[[nodiscard]]` results on fallible operations.

### `core/safe_numeric.hpp`

Purpose: make integer overflow and narrowing explicit.

Public surface:

| Method | Purpose |
| --- | --- |
| `checked_add(lhs, rhs)` | Adds integral values and returns `NumericError` on overflow/underflow. |
| `checked_sub(lhs, rhs)` | Subtracts integral values and returns `NumericError` on overflow/underflow. |
| `checked_mul(lhs, rhs)` | Multiplies integral values and returns `NumericError` on overflow/underflow. |
| `checked_narrow_cast<To>(value)` | Converts between integral types only if the target range can represent the value. |

Implementation notes:

- Uses compiler overflow builtins where available, with constexpr fallback logic.
- The helpers are used at financial and parser boundaries where undefined
  behavior would be unacceptable.

### `core/fixed_price.hpp`

Purpose: deterministic price storage using signed 64-bit fixed-point integers.

Public surface:

| Method | Purpose |
| --- | --- |
| `FixedPrice::from_raw(raw)` | Constructs from an already-scaled wire/internal value. |
| `FixedPrice::from_dollars_cents(dollars, cents)` | Convenience constructor returning zero on invalid input. Prefer `try_from_dollars_cents` in production paths. |
| `FixedPrice::try_from_dollars_cents(dollars, cents)` | Checked constructor with typed errors. |
| `FixedPrice::from_double(d)` | `consteval` helper for compile-time test constants only. |
| `to_raw()` | Returns the scaled integer representation. |
| `to_chars()` | Formats without heap allocation. |
| `to_string_view(buffer)` | Formats into caller-provided stack storage. |
| `operator+`, `operator-`, unary `operator-`, `operator*` | Convenience arithmetic returning zero on overflow. Prefer checked methods for production-critical arithmetic. |
| `operator+=`, `operator-=` | In-place convenience arithmetic. |
| `try_add`, `try_sub`, `try_negate`, `try_mul` | Checked arithmetic returning `FixedPriceError`. |
| `operator<=>`, `operator==` | Value comparison on raw fixed-point value. |
| `is_zero()` | Checks raw zero. |
| `penny_tick()`, `nickel_tick()` | Standard listed-options tick helpers. |

Implementation notes:

- Price display supports up to 8 decimal places but preserves at least two
  decimals for listed-options display conventions.
- Runtime code should avoid `from_double`; financial values should enter as
  integers or checked decimal parsers.

### `core/options_types.hpp`

Purpose: domain model for option specifications and post-trade Greeks.

Public surface:

| Method | Purpose |
| --- | --- |
| `OptionSpec::root_view()` | Returns the null-terminated root symbol slice without allocation. |
| `OptionSpec::dte(today_yyyymmdd)` | Simplified days-to-expiry approximation. |
| `Greeks::operator+=` | Aggregates per-contract Greeks. |
| `Greeks::operator*(size)` | Scales Greeks by position size. |
| `to_string(ExerciseStyle)` | Converts exercise style to stable text. |
| `to_string(SettlementType)` | Converts settlement type to stable text. |
| `to_string(OptionRight)` | Converts option right to stable text. |

Implementation notes:

- `double` is intentionally limited to Greeks because Greeks are analytics, not
  price storage or price comparison.
- `dte()` is explicitly marked as simplified; production code should replace it
  with calendar-aware date handling.

## Infrastructure Methods

### `infra/spsc_queue.hpp`

Purpose: lock-free bounded single-producer/single-consumer queue.

Public surface:

| Method | Purpose |
| --- | --- |
| `try_push(const T&)` | Producer enqueue by copy; returns false when full. |
| `try_push(T&&)` | Producer enqueue by move; returns false when full. |
| `try_emplace(args...)` | Constructs a value for enqueue with one explicit assignment into the slot. |
| `try_pop()` | Consumer dequeue returning `std::optional<T>`. |
| `try_pop(T& out)` | Consumer dequeue into caller storage to avoid optional overhead. |
| `size()` | Approximate queue depth for SPSC use. |
| `empty()` | Convenience empty check. |
| `capacity()` | Compile-time configured capacity. |

Implementation notes:

- `Capacity` must be a power of two and at least 2.
- Head and tail atomics are cache-line aligned to reduce false sharing.
- This is only correct for one producer and one consumer.

### `infra/lmax_disruptor.hpp`

Purpose: minimal LMAX Disruptor-style SPSC ring with explicit claim/publish and
consume/release phases.

Public surface:

| Method | Purpose |
| --- | --- |
| `try_claim()` | Producer reserves the next slot and receives a pointer to write the event. |
| `publish(sequence)` | Producer makes a claimed sequence visible to the consumer. |
| `try_consume()` | Consumer reads the next published record. |
| `release(sequence)` | Consumer advances after processing a record. |
| `published_sequence()` | Returns the latest visible producer sequence. |
| `consumer_sequence()` | Returns the released consumer sequence. |
| `size()` | Returns producer-consumer distance. |
| `capacity()` | Compile-time configured capacity. |

Implementation notes:

- This is not the full LMAX Disruptor: there are no multicast consumers,
  dependency graphs, or wait strategies.
- The type `T` must be default constructible because the ring is preallocated.

### `infra/seqlock.hpp`

Purpose: lock-free latest-value store for one writer and many readers, suitable
for NBBO snapshots.

Public surface:

| Method | Purpose |
| --- | --- |
| `store(value)` | Single writer publishes a new trivially-copyable value. |
| `load()` | Reader loads a consistent snapshot, retrying if it races a writer. |
| `load_with_seq()` | Reader loads a snapshot plus sequence for staleness detection. |
| `sequence()` | Returns current sequence; even means no write in progress. |

Implementation notes:

- `T` must be trivially copyable. This avoids unsafe copying of owning types.
- Writer protocol publishes odd/even sequence values around the data write.
- Readers can spin during writer activity; this is appropriate only when writes
  are short and readers need the latest value without locks.

### `infra/wal.hpp`

Purpose: in-memory write-ahead log for audit, deterministic replay, and parity
checks.

Public surface:

| Method | Purpose |
| --- | --- |
| `Wal(capacity)` | Reserves bounded storage for hot-path append. |
| `append(type, timestamp_ns, entity_id, payload_a, payload_b)` | Appends an event and returns assigned sequence or `BufferFull`. |
| `read(sequence)` | Reads an entry by sequence number, or null if unavailable. |
| `read_range(start_seq, count)` | Copies a contiguous sequence range. |
| `size()` | Number of stored entries. |
| `next_sequence()` | Next sequence that would be assigned. |
| `verify_parity(other)` | Compares deterministic outputs, returning the first divergent sequence on mismatch. |

Implementation notes:

- Capacity is now enforced. The prior `BufferFull` error existed but was not
  reachable, which allowed hidden vector growth after reserve.
- Sequence advances only after capacity is confirmed, so a failed append does
  not create a gap.
- Timestamps are intentionally excluded from parity because replay wall-clock
  times can differ while logical output remains deterministic.

### `infra/rate_limiter.hpp`

Purpose: lock-free token bucket for quote/order throttling.

Public surface:

| Method | Purpose |
| --- | --- |
| `try_consume()` | Consumes one token if available. |
| `try_consume(n)` | Consumes `n` tokens atomically if available. |
| `refill()` | Timer/control path restores tokens to the configured rate. |
| `set_rate(tokens_per_second)` | Hot-reloads the configured rate. |
| `remaining()` | Reads current token count. |
| `rate()` | Reads configured refill amount. |

Implementation notes:

- The hot path performs only atomic CAS work.
- Rate changes take effect on the next `refill()`.

### `infra/aeron_transport.hpp`

Purpose: document the mapping from these local primitives to Aeron IPC/UDP
concepts without taking a runtime dependency on Aeron.

Public surface:

| Type | Purpose |
| --- | --- |
| `AeronOfferResult` | Typed mapping of Aeron publication result codes. |
| `stream_ids::*` | Stable stream IDs for logical ATS message flows. |
| `AeronMsgHeader` | Fixed 32-byte application message header. |

Implementation notes:

- This is a design-reference header. It contains no live transport code.
- An unused `FixedPrice` include was removed to keep the header independent.

## Market Data Methods

### `market_data/opra.hpp`

Purpose: parse OCC/OPRA symbols and compact exchange quote CSV lines.

Public surface:

| Method | Purpose |
| --- | --- |
| `OpraCode::root_view()` | Returns root symbol without allocation. |
| `to_string(OpraParseError)` | Stable parser error names. |
| `parse_opra_code(code)` | Parses canonical 21-character OCC code. |
| `parse_exchange_quote_line(line)` | Parses `opra_code,timestamp,bid,ask,bid_qty,ask_qty,exchange`. |

Implementation notes:

- Decimal parsing uses checked arithmetic and rejects excess precision.
- Quantities must be positive and fit in `int32_t`.
- Crossed or non-positive bid/ask values are rejected.

### `market_data/order_book.hpp`

Purpose: correctness-first limit order book using price-time priority.

Public surface:

| Method | Purpose |
| --- | --- |
| `Order::is_fully_filled()` | True when remaining quantity is zero or negative. |
| `PriceLevel::total_quantity()` | Sums remaining quantity at a level. |
| `PriceLevel::empty()` | Checks whether level has no resting orders. |
| `OrderBook::add_order(order)` | Adds an order, matching first if it crosses the opposite side. |
| `OrderBook::cancel_order(order_id)` | Cancels a resting order and returns remaining quantity. |
| `best_bid()` | Highest bid if present. |
| `best_ask()` | Lowest ask if present. |
| `spread()` | Best ask minus best bid if both sides exist. |
| `bid_level_count()` / `ask_level_count()` | Number of price levels per side. |
| `order_count()` | Number of resting indexed orders. |
| `depth(side, max_levels)` | Price/quantity depth snapshot for a side. |

Implementation notes:

- `std::map` and `std::deque` are intentionally used for clarity and stable
  pointer behavior in this reference implementation.
- Production options books may replace this with preallocated tick-indexed
  arrays once the price ladder and latency budget justify it.

### `market_data/series_book.hpp`

Purpose: quote book for one options series where market makers are passive and
retail orders are aggressive.

Public surface:

| Method | Purpose |
| --- | --- |
| `update_quote(quote)` | Inserts/replaces one MM's latest two-sided quote. |
| `withdraw_quote(mm_id)` | Marks a market maker inactive and zeroes quote sizes. |
| `best_bid()` | Best active MM bid. |
| `best_ask()` | Best active MM ask. |
| `book_bid()` | FixedPrice best bid or zero. |
| `book_ask()` | FixedPrice best ask or zero. |
| `active_mm_count()` | Number of active market makers. |
| `match_retail_order(order, nbbo)` | Matches an aggressive retail order with pro-rata allocation and NBBO checks. |
| `match_retail_order_checked(order, nbbo, current_time_ns, max_staleness_ns)` | Rejects stale NBBO before matching. |
| `quote(mm_id)` | Read-only access to a market maker quote. |

Implementation notes:

- The book is a flat array of at most 16 MM quotes; linear scans are faster and
  simpler than heap/tree structures at this size.
- Retail buys match MM asks; retail sells match MM bids.
- Fills capture NBBO bid/ask for price improvement and audit reporting.
- The implementation returns fills even when liquidity is partial; callers must
  compare filled quantity to requested quantity when complete-fill semantics are
  required.

## OMS Methods

### `oms/fix_parser.hpp`

Purpose: parse the application-layer subset of FIX 4.4 needed for order entry.

Public surface:

| Method | Purpose |
| --- | --- |
| `to_string(FixParseError)` | Stable parser error names. |
| `parse_fix_new_order(msg)` | Parses SOH-delimited FIX into `FixNewOrder`. |

Internal helpers:

| Method | Purpose |
| --- | --- |
| `find_fix_tag(msg, tag_prefix)` | Linear scan for a tag value. |
| `parse_fix_int(value)` | Strict integer parser for quantities. |
| `parse_fix_price(value)` | Strict positive fixed-point decimal parser. |

Implementation notes:

- Price parsing now uses checked arithmetic and rejects zero, negative,
  overflowing, malformed, and over-precision values.
- Invalid `TimeInForce` values are rejected instead of silently defaulting.
- `std::string_view` fields reference the input message buffer; callers must
  keep that buffer alive or copy into owned storage before crossing threads.

### `oms/order_state_machine.hpp`

Purpose: encode allowed lifecycle transitions for a single order.

Public surface:

| Method | Purpose |
| --- | --- |
| `to_string(OrderState)` | Stable order state names. |
| `to_string(OrderEvent)` | Stable order event names. |
| `to_string(TransitionError)` | Stable transition error names. |
| `is_terminal(OrderState)` | Checks whether state is terminal. |
| `transition(current, event)` | Pure function for one state transition. |
| `OrderStateMachine::apply(event)` | Applies a valid transition to the tracked order. |
| `state()` | Current state. |
| `order_id()` | Tracked order identifier. |
| `transition_count()` | Number of successful transitions. |
| `is_terminal()` | Object-level terminal-state check. |

Implementation notes:

- Failed transitions do not mutate state.
- Terminal states reject further events.

### `oms/spread_state_machine.hpp`

Purpose: coordinate multi-leg order lifecycle state from per-leg events.

Public surface:

| Method | Purpose |
| --- | --- |
| `to_string(SpreadState)` | Stable spread state names. |
| `to_string(SpreadError)` | Stable spread error names. |
| `is_terminal(SpreadState)` | Checks spread terminal state. |
| `SpreadStateMachine::apply(event)` | Applies one leg event and recomputes composite state. |
| `submit_all()` | Submits all legs from `New`. |
| `cancel_all()` | Requests cancel for active/cancellable legs. |
| `composite_state()` | Computes aggregate spread state from leg states. |
| `spread_id()` | Tracked spread identifier. |
| `leg_count()` | Number of legs. |
| `leg_state(index)` | State of one leg. |
| `terminal_leg_count()` | Count of terminal legs. |
| `filled_leg_count()` | Count of filled legs. |

Implementation notes:

- Mixed filled/cancelled/rejected terminal outcomes become `LegDivergence`,
  requiring manual review.
- Tests now validate fallible spread operations instead of ignoring `expected`
  return values.

## Risk Methods

### `risk/pre_trade.hpp`

Purpose: lock-free synchronous risk checks for position, notional, and order
rate limits.

Public surface:

| Method | Purpose |
| --- | --- |
| `to_string(RiskReject)` | Stable rejection names. |
| `check_position(delta)` | Atomically applies a position delta if within limit. |
| `check_notional(notional_delta_raw)` | Atomically applies notional delta if within limit. |
| `check_order_rate()` | Atomically consumes one order-rate slot. |
| `update_limits(limits)` | Hot-reloads risk limits. |
| `reset_order_count()` | Resets per-second order counter. |
| `net_position()` | Reads current net position. |
| `net_notional_raw()` | Reads current notional exposure. |
| `order_count()` | Reads current order count. |

Implementation notes:

- Each mutable counter uses compare-exchange to avoid locks.
- Limit staleness is intentionally acceptable for the hot path; updates become
  visible on subsequent checks.
- Tests verify concurrent position updates and now record any failed expected
  result explicitly.

### `risk/eq_engine.hpp`

Purpose: execution-quality scoring for market makers.

Public surface:

| Method | Purpose |
| --- | --- |
| `EqMetrics::fill_rate_bps()` | Computes integer fill rate in basis points. |
| `EqMetrics::avg_improvement_raw()` | Computes average raw fixed-point price improvement. |
| `EqMetrics::avg_response_ns()` | Computes average response time. |
| `EqWeights::valid()` | Ensures weights sum to 10000 bps. |
| `to_string(EqError)` | Stable EQ error names. |
| `record_fill(mm_id, fill_price, nbbo_price, response_ns)` | Adds a fill observation. |
| `record_opportunity(mm_id)` | Adds an opportunity observation. |
| `compute_scores(weights)` | Normalizes peer metrics and ranks market makers. |
| `metrics(mm_id)` | Reads raw metrics for one market maker. |
| `reset()` | Clears the scoring window. |

Implementation notes:

- Scores are relative to peers in the current window, not absolute quality
  guarantees.
- Fill-rate and weights use basis points to avoid floating-point scoring.
- Response-time scoring is inverted: lower latency is better.

## End-to-End Flow

A typical order path is:

1. Parse inbound FIX with `parse_fix_new_order()`.
2. Apply OMS state with `OrderStateMachine::apply(OrderEvent::Submit)`.
3. Run `PreTradeRiskCheck` for position, notional, and order-rate gates.
4. Update the `SeriesBook` with current MM quotes and read current NBBO from a
   `Seqlock<NbboEntry>` or equivalent snapshot store.
5. Match retail flow using `SeriesBook::match_retail_order_checked()`.
6. Append order/fill/reject events to `Wal::append()`.
7. Feed fills/opportunities into `EqEngine` for periodic MM scoring.
8. Verify deterministic behavior with `Wal::verify_parity()` during replay.

## Validation Commands

Use these commands from the repo root:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The cleaned verification run for this guide used:

```bash
cmake --build build-clean -j
ctest --test-dir build-clean --output-on-failure
```

Result: 167/167 tests passed.

## Current Cleanup Decisions

The following code was changed during the cleanup pass:

- Removed an unused include from `infra/aeron_transport.hpp`.
- Made `Wal::BufferFull` reachable by enforcing constructor capacity before
  append, preventing hidden vector growth in a hot-path primitive.
- Hardened FIX price parsing with checked arithmetic and explicit invalid TIF
  rejection.
- Updated tests to handle `[[nodiscard]]` fallible results and added regression
  coverage for WAL capacity and FIX invalid-price cases.

No public examples, duplicate test frameworks, or governance files were removed.
They are intentional project surfaces: examples verify consumer usage, Catch2 and
GoogleTest demonstrate framework parity, and governance files support public
collaboration and release controls.
