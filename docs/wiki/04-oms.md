# OMS

Primary entry points:
- FIX parsing (subset): `src/oms/fix_parser.hpp`
- Order state machine: `src/oms/order_state_machine.hpp`
- Spread state machine: `src/oms/spread_state_machine.hpp`

Intent:
- Parse external inbound order flow (FIX) with strict validation.
- Encode order/spread lifecycle transitions as state machines so invalid event
  sequences are rejected deterministically.

The state machines are designed to be test-first:
- unit tests cover happy paths and edge cases
- terminal states reject further transitions

