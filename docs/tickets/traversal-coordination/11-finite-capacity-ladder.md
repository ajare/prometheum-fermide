# 11 — Enforce finite capacity on ladder transit

**Priority:** P1

**Difficulty:** hard

**What to build:** Use a ladder as the first complete finite-capacity traversal slice. Agents request admission, reserve spaced climbing positions, enter the ladder transit, climb at the correct speed, and release occupancy only after leaving.

**Blocked by:**
- 04 — Execute ordinary edges through the traversal protocol
- 10 — Recover from full queues, expired permits, and excessive waits

**Status:** complete

- [x] Ladder capacity is derived from usable length and configured agent spacing.
- [x] Occupants plus admission reservations never exceed capacity.
- [x] Each admitted agent owns a distinct climbing-position reservation.
- [x] Waiting agents cannot enter merely because the ladder edge appears in their path.
- [x] Ladder movement uses climbing speed and correct distance-over-speed route cost.
- [x] Boarding the ladder transit and leaving it commit sector membership at their crossing endpoints.
- [x] Cancellation before entry releases capacity; cancellation while occupying preserves occupancy until safe exit.
- [x] Simultaneous requests are resolved deterministically.
- [x] Capacity, occupants, and reservations are exposed in diagnostic snapshots.
