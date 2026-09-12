# 04 — Execute ordinary edges through the traversal protocol

**Priority:** P0

**Difficulty:** hard

**What to build:** Introduce the agent traversal task and generic request-to-commit lifecycle using an ordinary edge as the first tracer bullet. Paths remain route intent, while every transition explicitly obtains permission—even when an unconstrained edge grants it immediately.

**Blocked by:** 03 — Expand typed identity and central ownership

**Status:** ready-for-agent

- [ ] An agent owns at most one active locomotion task.
- [ ] Reaching an edge creates a traversal request instead of directly changing sector membership.
- [ ] An unconstrained edge grants an immediate traversal permit through the generic protocol.
- [ ] A denied edge cannot be crossed merely because it appears in the path.
- [ ] The agent moves itself visibly between configured transition endpoints while holding the permit.
- [ ] The agent remains in the source sector until reaching the destination endpoint.
- [ ] Sector membership commits atomically when crossing completes.
- [ ] Cancellation before commit releases the request and permit without changing sectors.
- [ ] Player-directed and autonomous movement use the same execution path.
