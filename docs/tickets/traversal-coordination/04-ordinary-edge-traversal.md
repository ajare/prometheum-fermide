# 04 — Execute ordinary edges through the traversal protocol

**Priority:** P0

**Difficulty:** hard

**What to build:** Introduce the agent traversal task and generic request-to-commit lifecycle using an ordinary edge as the first tracer bullet. Paths remain route intent, while every transition explicitly obtains permission—even when an unconstrained edge grants it immediately.

**Blocked by:** 03 — Expand typed identity and central ownership

**Status:** complete

- [x] An agent owns at most one active locomotion task.
- [x] Reaching an edge creates a traversal request instead of directly changing sector membership.
- [x] An unconstrained edge grants an immediate traversal permit through the generic protocol.
- [x] A denied edge cannot be crossed merely because it appears in the path.
- [x] The agent moves itself visibly between configured transition endpoints while holding the permit.
- [x] The agent remains in the source sector until reaching the destination endpoint.
- [x] Sector membership commits atomically when crossing completes.
- [x] Cancellation before commit releases the request and permit without changing sectors.
- [x] Player-directed and autonomous movement use the same execution path.
