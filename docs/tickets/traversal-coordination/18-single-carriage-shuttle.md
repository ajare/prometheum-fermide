# 18 — Complete a single-carriage shuttle journey

**Priority:** P1

**Difficulty:** hard

**What to build:** Reuse the transport journey model for a linear single-carriage shuttle. Passengers call it, queue, board with finite capacity, select destinations, travel horizontally under LOOK scheduling, and disembark through correctly aligned doors.

**Blocked by:** 17 — Recover lift journeys from failure and cancellation

**Status:** complete

- [x] A shuttle route remains a static transit while the carriage manifest identifies exact occupancy.
- [x] Landing interaction activates pickup demand only after successful physical use or reuse of an active call.
- [x] Boarding and disembarking obey the same capacity, permit, and departure barriers as a lift.
- [x] Passenger positions remain local to the moving carriage.
- [x] Landing and carriage doors open only when correctly aligned and stationary.
- [x] Linear shuttle stop requests use LOOK scheduling with fair idle dispatch.
- [x] Full-capacity and disembark-before-embark scenarios preserve waiting priority.
- [x] Cancellation and deactivation reach the same safe outcomes as lift journeys.
- [x] Existing shuttle rendering and world construction use the new authoritative state.
