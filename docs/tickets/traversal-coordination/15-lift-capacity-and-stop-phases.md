# 15 — Coordinate lift capacity, disembarkation, and boarding

**Priority:** P1

**Difficulty:** hard

**What to build:** Extend the lift journey to several passengers competing for finite capacity. A stop coordinator gives disembarkers priority, admits a bounded set of boarders, serializes new destination confirmations, and closes only after the departure barrier is satisfied.

**Blocked by:**
- 08 — Queue agents fairly on both sides of a door
- 14 — Complete a two-stop lift journey for one passenger

**Status:** complete

- [x] Declared lift capacity is validated against generated interior standing positions.
- [x] Occupants plus boarding reservations never exceed capacity under simultaneous requests.
- [x] Passengers leaving at the stop receive crossing access before any boarder.
- [x] Capacity remains occupied until disembark crossing completes.
- [x] Waiting passengers incompatible with the current run remain queued.
- [x] New destination selections are serialized in deterministic boarding order.
- [x] An already-active destination confirms immediately while adding the passenger as a request owner.
- [x] Minimum dwell prevents premature closure.
- [x] Maximum boarding cutoff stops issuing new reservations while honoring existing ones.
- [x] A full lift departs and unadmitted passengers keep their queue tickets and pickup demand.
- [x] Doors close only when crossings, accepted boarders, and destination confirmations are resolved.
