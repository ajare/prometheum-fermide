# 06 — Traverse manual and automatic doors safely

**Priority:** P0

**Difficulty:** hard

**What to build:** Migrate an ordinary doorway to a traversal resource and let one agent complete end-to-end journeys through manual and automatic door activation modes. The route must wait for a fully open door, cross visibly, and allow the door to close afterward.

**Blocked by:**
- 04 — Execute ordinary edges through the traversal protocol
- 05 — Prove typed interaction and device operations

**Status:** ready-for-agent

- [ ] Door construction selects an explicit automatic, manual, remote-controlled, or unavailable activation mode.
- [ ] A manual-door journey makes the agent reach and operate the door before crossing.
- [ ] An automatic-door journey turns coordinated presence into an opening request.
- [ ] An unavailable door rejects traversal and cannot be bypassed by path execution.
- [ ] Crossing permission is granted only after the door is fully open.
- [ ] The door remains open for the complete visible crossing.
- [ ] The door closes after a configurable hold-open period once no traversal requires it.
- [ ] Path completion, sector membership, door state, and operation status are externally observable.
- [ ] The migrated door edge has no simultaneous legacy movement authority.
