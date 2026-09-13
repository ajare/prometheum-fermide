# 22 — Rebuild traversal topology safely while paused

**Priority:** P2

**Difficulty:** hard

**What to build:** Allow structural building edits to replace affected graph and resource topology only while simulation ticks are paused. Active ownership is cancelled or brought to a safe boundary, rebuilt configuration is validated, and simulation resumes without stale handles.

**Blocked by:**
- 13 — Hold extensible ladders and force bridges safely
- 17 — Recover lift journeys from failure and cancellation
- 19 — Coordinate a multi-carriage shuttle and its access zones
- 20 — Migrate open platform lifts
- 21 — Migrate bulkhead doors and window thresholds

**Status:** implemented

- [x] Structural edits are rejected while active simulation ticks continue.
- [x] Pausing initiates deterministic cancellation or safe completion for affected traversal tasks.
- [x] Queue tickets, position reservations, permits, leases, manifests, and stop-request ownership are cleaned before replacement.
- [x] Rebuilt edges reference exactly one valid traversal authority.
- [x] Queue geometry, controls, capacity positions, stops, and transport doors are revalidated.
- [x] Removed handles fail safely and cannot resolve to newly created entities accidentally.
- [x] Unaffected agents and resources preserve their state.
- [x] Simulation can resume and complete new paths through the rebuilt topology.
- [x] Validation failure leaves the simulation paused with a useful diagnostic rather than a partially active graph.
