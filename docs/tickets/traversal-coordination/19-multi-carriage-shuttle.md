# 19 — Coordinate a multi-carriage shuttle and its access zones

**Priority:** P1

**Difficulty:** hard

**What to build:** Extend the shuttle to a coupled vehicle with independently capacity-owning carriages and multiple doors. Stop coordinators preserve platform fairness while assigning passengers to available carriages and balancing disembark crossings.

**Blocked by:** 18 — Complete a single-carriage shuttle journey

**Status:** complete

- [x] The complete shuttle owns one motion state and schedule while each carriage owns capacity, reservations, occupants, positions, and doors.
- [x] One logical boarding queue exists per stop, connected access zone, and travel direction.
- [x] Doors on the same connected platform share logical priority; disconnected approaches remain separate.
- [x] The coordinator assigns an available carriage and door by shortest walk, then least occupancy, then stable tie-breaker.
- [x] Assignment binds the passenger’s capacity reservation and physical queue position to that carriage and door.
- [x] Reassigning an eligible door does not change logical queue priority.
- [x] No carriage exceeds its individual capacity.
- [x] Disembark doors are chosen by shortest interior route and active crossing load.
- [x] Multiple door crossings remain within per-threshold lane limits and complete before departure.
- [x] Coalesced stop requests and LOOK scheduling remain vehicle-wide rather than carriage-specific.
