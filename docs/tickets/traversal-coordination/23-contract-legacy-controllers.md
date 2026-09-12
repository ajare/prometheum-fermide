# 23 — Contract and remove the legacy controller hierarchy

**Priority:** P1

**Difficulty:** hard

**What to build:** Complete the expand–migrate–contract refactor after every resource has a replacement authority. Move remaining construction, UI, rendering, and diagnostics to explicit interaction and traversal concepts, complete central ownership, then delete the ambiguous legacy coordination hierarchy.

**Blocked by:**
- 05 — Prove typed interaction and device operations
- 13 — Hold extensible ladders and force bridges safely
- 17 — Recover lift journeys from failure and cancellation
- 19 — Coordinate a multi-carriage shuttle and its access zones
- 20 — Migrate open platform lifts
- 21 — Migrate bulkhead doors and window thresholds
- 22 — Rebuild traversal topology safely while paused

**Status:** ready-for-agent

- [ ] Every graph edge has exactly one replacement traversal authority or immediate-permit policy.
- [ ] Every physical control uses interaction points and typed command bindings.
- [ ] Every stateful device reports queryable operations without legacy completion callbacks.
- [ ] Agents no longer inherit from a device-controller or usability base.
- [ ] Building-owned registries are the authoritative lifetime owners of migrated simulation entities.
- [ ] UI and rendering consume stable IDs and read-only state rather than legacy mutable pointers.
- [ ] The old usability, controller, controllable, orchestrator, orchestrated-system, and vertex-controller implementations have no remaining call sites.
- [ ] Legacy types and construction adapters are deleted rather than retained as aliases.
- [ ] Debug and Release application and headless builds remain green.
- [ ] All migrated behavioural scenarios still pass after deletion.
