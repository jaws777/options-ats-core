# Core Types

This page is a narrative map of the `src/core/` public surface.

Key types:
- `core::FixedPrice` (`src/core/fixed_price.hpp`): signed 64-bit fixed-point
  storage for prices (scale 1e8). No floating point for price comparison.
- `core::checked_add/sub/mul` (`src/core/safe_numeric.hpp`): checked arithmetic
  that makes overflow/underflow explicit at the call site.
- `core::expected` / `core::unexpected` (`src/core/expected.hpp`): C++23
  `std::expected` compatibility layer, without putting new types in `namespace std`.
- `core::OptionSpec` and `core::Greeks` (`src/core/options_types.hpp`): domain
  structures for options series and post-trade risk analytics.

Why it matters:
- Price correctness is existential in trading systems. Fixed-point + checked
  math eliminates silent rounding and UB paths.
- Explicit error typing makes ingestion boundaries (FIX, OPRA, etc.) defensible.

