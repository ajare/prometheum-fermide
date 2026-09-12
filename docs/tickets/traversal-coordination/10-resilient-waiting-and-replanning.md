# 10 — Recover from full queues, expired permits, and excessive waits

**Priority:** P0

**Difficulty:** hard

**What to build:** Make waiting robust when physical queue lanes are full, assigned goals cannot be reached, permits expire, or another route becomes materially faster. Agents retain only compatible logical priority and release scarce reservations promptly.

**Blocked by:**
- 08 — Queue agents fairly on both sides of a door
- 09 — Enforce door safety and wide-threshold concurrency

**Status:** ready-for-agent

- [ ] Agents may hold logical queue tickets while every physical position is occupied.
- [ ] Overflow agents remain upstream and do not reserve destination capacity or crossing lanes.
- [ ] Lack of progress toward an assigned goal releases short-lived reservations and requests another valid position.
- [ ] Admission and crossing permits expire deterministically and are reassigned.
- [ ] A compatible replan at the same immediate resource retains queue priority.
- [ ] A replan with incompatible origin, direction, or eligibility releases the old ticket completely.
- [ ] Path estimates include stable preparation and queue-delay costs without creating reservations.
- [ ] After a minimum wait, replanning occurs only when an alternative ETA wins by a configured margin.
- [ ] Agents never replan while holding an active admission or crossing permit.
- [ ] No cancellation, timeout, or failed assignment leaves a ghost reservation.
