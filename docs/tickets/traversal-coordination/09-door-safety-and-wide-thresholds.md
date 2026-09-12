# 09 — Enforce door safety and wide-threshold concurrency

**Priority:** P0

**Difficulty:** hard

**What to build:** Complete the door resource with scoped open leases, sensor observations, safe closing and reopening, multiple crossing lanes for wide thresholds, and graceful deactivation. Agents can use available width concurrently without permitting unsafe closure.

**Blocked by:** 08 — Queue agents fairly on both sides of a door

**Status:** complete

- [x] Preparation, active crossings, and external hold-open state use independently owned open leases.
- [x] A close command is rejected while any open lease or obstruction remains.
- [x] Presence and obstruction sensors publish observations to the coordinator rather than commanding the door directly.
- [x] An ordinary closing door reopens for a new valid request or obstruction.
- [x] Configured wide thresholds expose the correct number of independent crossing lanes.
- [x] Concurrent crossings never exceed lane count and all lanes share the door safety state.
- [x] Disabling the door rejects new permits while allowing already-active crossings to finish safely.
- [x] After active crossings finish, pending requests fail with reasons that allow replanning.
- [x] Repeated scenarios produce deterministic lane allocation and event order.
