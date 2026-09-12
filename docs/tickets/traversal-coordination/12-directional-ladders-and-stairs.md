# 12 — Coordinate opposing ladder and narrow-stair traffic

**Priority:** P1

**Difficulty:** hard

**What to build:** Extend finite-capacity traversal with directional admission and bounded batches. Compatible agents share a ladder, opposing agents wait fairly, normal staircases remain unconstrained, and explicitly narrow stairs opt into the same coordination policy.

**Blocked by:** 11 — Enforce finite capacity on ladder transit

**Status:** ready-for-agent

- [ ] A ladder admits multiple agents moving in its active direction up to capacity.
- [ ] No opposing-direction agent is admitted while an occupant or committed entry remains.
- [ ] When opposite demand exists, new same-direction admission stops after the configured batch limit.
- [ ] Direction switches only after current occupants and committed entries drain.
- [ ] The oldest waiting direction receives the next batch, with deterministic tie-breaking.
- [ ] Continuous arrivals from one side cannot starve the other side.
- [ ] An ordinary staircase still permits unconstrained bidirectional movement.
- [ ] A configured narrow staircase enforces the same directional-capacity invariants.
- [ ] Direction, batch count, occupancy, and waiting demand are visible in diagnostics.
