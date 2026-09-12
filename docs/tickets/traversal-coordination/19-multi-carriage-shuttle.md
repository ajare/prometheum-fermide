# 19 — Coordinate a multi-carriage shuttle and its access zones

**Priority:** P1

**Difficulty:** hard

**What to build:** Extend the shuttle to a coupled vehicle with independently capacity-owning carriages and multiple doors. Stop coordinators preserve platform fairness while assigning passengers to available carriages and balancing disembark crossings.

**Blocked by:** 18 — Complete a single-carriage shuttle journey

**Status:** ready-for-agent

- [ ] The complete shuttle owns one motion state and schedule while each carriage owns capacity, reservations, occupants, positions, and doors.
- [ ] One logical boarding queue exists per stop, connected access zone, and travel direction.
- [ ] Doors on the same connected platform share logical priority; disconnected approaches remain separate.
- [ ] The coordinator assigns an available carriage and door by shortest walk, then least occupancy, then stable tie-breaker.
- [ ] Assignment binds the passenger’s capacity reservation and physical queue position to that carriage and door.
- [ ] Reassigning an eligible door does not change logical queue priority.
- [ ] No carriage exceeds its individual capacity.
- [ ] Disembark doors are chosen by shortest interior route and active crossing load.
- [ ] Multiple door crossings remain within per-threshold lane limits and complete before departure.
- [ ] Coalesced stop requests and LOOK scheduling remain vehicle-wide rather than carriage-specific.
