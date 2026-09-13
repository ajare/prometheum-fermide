# 12 — Coordinate opposing ladder and narrow-stair traffic

**Priority:** P1

**Difficulty:** hard

**What to build:** Extend finite-capacity traversal with directional admission and bounded batches. Compatible agents share a ladder, opposing agents wait fairly, normal staircases remain unconstrained, and explicitly narrow stairs opt into the same coordination policy.

**Blocked by:** 11 — Enforce finite capacity on ladder transit

**Status:** complete

- [x] A ladder admits multiple agents moving in its active direction up to capacity.
- [x] No opposing-direction agent is admitted while an occupant or committed entry remains.
- [x] When opposite demand exists, new same-direction admission stops after the configured batch limit.
- [x] Direction switches only after current occupants and committed entries drain.
- [x] The oldest waiting direction receives the next batch, with deterministic tie-breaking.
- [x] Continuous arrivals from one side cannot starve the other side.
- [x] An ordinary staircase still permits unconstrained bidirectional movement.
- [x] A configured narrow staircase enforces the same directional-capacity invariants.
- [x] Direction, batch count, occupancy, and waiting demand are visible in diagnostics.
