# 02 — Add the deterministic simulation seam

**Priority:** P0

**Difficulty:** medium

**What to build:** Make simulation progress independent of rendering by introducing a fixed timestep and an explicit public headless seam. A scenario can advance ticks, inspect read-only snapshots, and consume value-based events, producing identical results for identical inputs.

**Blocked by:** 01 — Establish a coherent headless build baseline

**Status:** complete

- [x] The simulation advances in fixed ticks while the graphical application uses accumulated render time.
- [x] Simulation work runs in documented phases: resource advancement, intent collection, allocation, movement, commit, and cleanup/event publication.
- [x] A public read-only snapshot exposes agent identity, sector, position, path state, and tick number.
- [x] State changes can be observed as value-based events without callbacks mutating simulation state re-entrantly.
- [x] Stable IDs provide deterministic ordering when entities act in the same tick.
- [x] Running the smoke scenario repeatedly produces identical snapshots and events.
- [x] Rendering observes simulation state but does not determine operation completion.
