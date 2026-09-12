# 08 — Queue agents fairly on both sides of a door

**Priority:** P0

**Difficulty:** hard

**What to build:** Add logical queue tickets and generated physical queue positions to door traversal. Multiple agents approach from both sides, stand separately, advance toward the threshold, and cross in deterministic fair order without a coordinator directly moving them.

**Blocked by:** 07 — Operate a remote-controlled door through its button

**Status:** ready-for-agent

- [ ] Each door approach defines and validates a queue lane by origin, direction, and extent.
- [ ] Logical queue tickets are distinct from physical queue-position reservations.
- [ ] Waiting agents occupy non-overlapping generated positions and move themselves when reassigned.
- [ ] FIFO order is preserved within each approach side.
- [ ] The oldest eligible queue head across both sides receives the next crossing lane, with stable-ID tie-breaking.
- [ ] An assigned operator can leave its queue position without losing logical priority.
- [ ] Cancelling or removing a queued agent releases every physical reservation and advances the remaining queue.
- [ ] Two-sided contention eventually serves both sides without starvation.
- [ ] Queue and crossing ownership are visible in read-only diagnostics.
