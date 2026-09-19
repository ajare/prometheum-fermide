# Extract simulation coordination from Building into SimulationCoordinator

Status: accepted

`src/core/Building.cpp` had grown to ~10,600 lines mixing two unrelated jobs: structural editing of the world (adding/removing sectors, objects, walls) and running the simulation (traversal requests, permits, queues, device operations, interactions, agent lifecycle, tick pipeline). The compiler choked on the translation unit under high analysis settings (issue #56). All simulation behaviour moves into a new `SimulationCoordinator` class owned by `Building`; `Building` keeps the world structure, owns every entity registry per ADR 0001, and acts as the facade (the design pattern — not the Facade sector of CONTEXT.md) through which all callers, including `Agent`, reach the coordinator. This was chosen over a same-class file split (the `BuildingSerialization.cpp` precedent) because a named mechanism shrinks the header as well as the source and gives the coordination code a boundary of its own.

## Considered options

- **File-split only (`BuildingCoordination.cpp`), same class.** Rejected: fixes the translation-unit size but leaves a 1,570-line header and keeps two unrelated responsibilities in one class; the facade arrangement costs little more and names the mechanism.
- **Pimpl behind Building.** Rejected: hides the coordinator entirely, making it untestable in isolation and adding an indirection layer without a consumer who needs the opacity.
- **Move the entity registries into the coordinator.** Rejected: contradicts ADR 0001 ("Building owns simulation entities") and drags serialisation and construction-record replay along for no behavioural gain.
- **Let `Agent` hold a direct coordinator reference.** Rejected: leaks the facade; `Agent.cpp` keeps calling `Building`, which forwards.
- **One big-bang move.** Rejected: ~6,000 lines moving at once is unreviewable; the work proceeds in staged moves leaf-first — agent lifecycle, then interactions/device operations, then the door/lift/shuttle allocation families, then the queue/admission core, then the tick pipeline and snapshots last — so each stage compiles against a Building that still owns the rest, builds green, and passes the headless smoke test. Extracting the tick pipeline first was rejected because everything else is called by it, which would force the facade surface to be guessed upfront.

## Consequences

- `SimulationCoordinator` becomes a friend of `Agent` and of the coordination types in `Coordination.h` alongside the existing `friend class Building` declarations; tightening that encapsulation later is a separate decision.
- The name deliberately avoids "controller", which CONTEXT.md bans, and echoes ADR 0001's "traversal coordination" vocabulary while covering the wider simulation scope (interactions, device operations, agent lifecycle, tick phases).
- `BuildingSerialization.cpp` and construction-record replay are untouched: no state moves, only behaviour.
- The coordinator's implementation is split per seam (`SimulationCoordinator.cpp`, `SimulationCoordinatorLifts.cpp`, …) behind a single `include/core/SimulationCoordinator.h`, following the `BuildingSerialization.cpp` precedent, so no translation unit recreates the size problem this refactor removes.
- Docs and code comments must write "facade (design pattern)" or "Building remains the facade" in full when describing this arrangement, to avoid collision with the Facade sector type.
