# 16 — Schedule multi-stop lifts with LOOK

**Priority:** P1

**Difficulty:** hard

**What to build:** Make a multi-stop lift serve coalesced landing and destination requests predictably. It continues in its current direction while demand remains ahead, reverses when appropriate, and dispatches fairly when idle.

**Blocked by:**
- 10 — Recover from full queues, expired permits, and excessive waits
- 15 — Coordinate lift capacity, disembarkation, and boarding

**Status:** complete

- [x] Landing and onboard demand for the same stop coalesce into one stop request with individual owners.
- [x] Removing one owner leaves a stop active while another owner still requires it.
- [x] A moving lift serves requested stops ahead before reversing direction.
- [x] Requests behind the current run remain scheduled for the return direction.
- [x] An idle lift chooses the oldest outstanding request, with distance and stop ID as tie-breakers.
- [x] Non-uniform stop spacing uses actual stop positions and never confuses stop index with travelled distance.
- [x] Waiting trip intents determine direction compatibility despite non-directional landing buttons.
- [x] Lift-edge estimates account for current position, schedule, preparation, and queue demand without reserving service.
- [x] Repeated identical request sequences produce identical stop and event order.
