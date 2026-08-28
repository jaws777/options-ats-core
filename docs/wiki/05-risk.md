# Risk

Primary entry points:
- Pre-trade checks: `src/risk/pre_trade.hpp`
- Execution quality scoring: `src/risk/eq_engine.hpp`

Pre-trade risk (`PreTradeRiskCheck`):
- lock-free atomic counters on the hot path
- CAS loops to apply deltas without mutex contention
- hot-reloadable limits via atomics

EQ scoring (`EqEngine`):
- aggregates fill/opportunity observations
- normalizes per-window performance to 0-10000 bps scales
- returns ranked per-MM scores used by the control plane / routing logic

