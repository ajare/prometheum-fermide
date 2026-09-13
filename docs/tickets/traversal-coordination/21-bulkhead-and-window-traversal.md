# 21 — Migrate bulkhead doors and window thresholds

**Priority:** P2

**Difficulty:** medium

**What to build:** Finish the remaining threshold-style resources. Bulkhead doors use ordinary door activation, queues, and safety regardless of geometric orientation; windows allow configured traversal only while fully open.

**Blocked by:**
- 09 — Enforce door safety and wide-threshold concurrency
- 10 — Recover from full queues, expired permits, and excessive waits

**Status:** implemented

- [x] Same-layer and same-height bulkhead edges still require traversal permission.
- [x] Bulkheads support configured activation modes, queue lanes, crossing lanes, open leases, and safe closure.
- [x] Bulkhead crossings cannot bypass a required control through direct opening.
- [x] A fully open window can grant traversal through its configured threshold.
- [x] Closed, opening, closing, tinted, frosted, and broken windows do not grant normal traversal.
- [x] Window and bulkhead cancellation releases every owned reservation and permit.
- [x] Route planning represents conditionally usable thresholds without reserving them.
- [x] Both resource types have deterministic one-agent and contention scenarios where applicable.
- [x] Their migrated edges no longer use legacy vertex control.
