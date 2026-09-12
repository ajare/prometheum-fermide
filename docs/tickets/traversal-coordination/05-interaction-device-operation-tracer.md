# 05 — Prove typed interaction and device operations

**Priority:** P0

**Difficulty:** medium

**What to build:** Replace one non-traversal control path, such as an agent-operated lighting control, with an interaction point, explicit actor identity, typed desired-state commands, and queryable device operations. This proves the replacement for usability inheritance and global pointer routing before doors depend on it.

**Blocked by:** 03 — Expand typed identity and central ownership

**Status:** complete

- [x] An explicit interaction task makes an agent walk within reach and reserve the control position.
- [x] A physical interaction point permits one active user by default for a configurable duration.
- [x] The agent is passed as an actor identity and does not need to inherit from a device-controller type.
- [x] The interaction emits a typed desired-state command rather than a generic toggle.
- [x] The command returns a queryable, cancellable operation with meaningful terminal status.
- [x] Required and best-effort bindings produce the specified aggregate interaction result.
- [x] Equivalent concurrent requests reuse active work and retain individual requester ownership.
- [x] Cancelling one requester does not cancel work still required by another.
- [x] The resulting state and operation outcome are visible through snapshots or events.
