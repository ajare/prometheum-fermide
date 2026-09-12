# 06 — Traverse manual and automatic doors safely

**Priority:** P0

**Difficulty:** hard

**What to build:** Migrate an ordinary doorway to a traversal resource and let one agent complete end-to-end journeys through manual and automatic door activation modes. The route must wait for a fully open door, cross visibly, and allow the door to close afterward.

**Blocked by:**
- 04 — Execute ordinary edges through the traversal protocol
- 05 — Prove typed interaction and device operations

**Status:** complete

- [x] Door construction selects an explicit automatic, manual, remote-controlled, or unavailable activation mode.
- [x] A manual-door journey makes the agent reach and operate the door before crossing.
- [x] An automatic-door journey turns coordinated presence into an opening request.
- [x] An unavailable door rejects traversal and cannot be bypassed by path execution.
- [x] Crossing permission is granted only after the door is fully open.
- [x] The door remains open for the complete visible crossing.
- [x] The door closes after a configurable hold-open period once no traversal requires it.
- [x] Path completion, sector membership, door state, and operation status are externally observable.
- [x] The migrated door edge has no simultaneous legacy movement authority.
