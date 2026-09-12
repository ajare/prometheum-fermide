# Agent movement and coordination

## Scope and status

This document describes the current movement implementation and compares it with the intended rules for shared resources. It is based on a source audit of `include/core` and `src/core`.

The project now builds with the installed MSVC in both Debug and Release. Build compatibility required replacing fmt 9.1's removed `stdext::checked_array_iterator` workaround and making `Useable` a shared virtual base of `Controllable` and `Controller`. There is no automated simulation test suite, so the movement findings below remain source-confirmed gaps rather than claims from a new end-to-end reproduction.

## Executive summary

The code has three mostly separate mechanisms:

1. **Route planning** builds a `Path` through graph `Vertex` and `Edge` objects.
2. **Local movement** makes an `Agent` walk directly toward successive path vertices.
3. **Coordination** is attempted at vertices that have a `VertexController`, principally doors.

Device orchestration (`Orchestrator`, lift/shuttle orchestrated systems, and `RailedTransport`) controls buttons, doors, and vehicle animation. It does **not** currently coordinate riders. The only implemented multi-agent behaviour is the one-dimensional queue in `VertexControllerArea`.

The most important architectural gap is that a path edge is not treated as a permission boundary. `Agent` never calls `Edge::isTraversable()` or `Edge::requestTraversal()`. Most transport edges have no traversal controller, capacity is not enforced, and agents are not attached to moving lift or shuttle cars. This makes correct lift embark/disembark coordination impossible in the current execution model even though route finding can produce the right route.

## Conceptual map

```text
Building
├── Layer (fore/back)
│   └── Sector
│       ├── Location (room/corridor)
│       └── Transit (lift/shuttle/ladder/staircase)
├── Graph
│   ├── Vertex (place on a route)
│   └── Edge (possible transition and pathfinding cost)
├── Agent (path follower)
├── VertexController (threshold coordinator)
│   └── VertexControllerArea (one side's queue and movement roles)
└── Orchestrator (device-action router)
    └── OrchestratedSystem (button ↔ door/lift/shuttle wiring)
```

Two similarly named concepts must be kept distinct:

- `Controller` / `Orchestrator` handle **device use**.
- `VertexController` / `VertexControllerArea` handle **agent movement at a threshold**.

The `Orchestrator` is not a group movement coordinator.

## Current responsibilities

### `Building`, `Sector`, and update order

`Building` owns sectors, the graph, the device orchestrator, agents, and traversal controllers. Agents are also members of exactly one `Sector`'s `mAgents` set.

Each frame (`src/core/Building.cpp:2243-2255`):

1. Every sector updates.
2. Sector objects update, then agents belonging to that sector update (`src/core/Sector.cpp:506-519`).
3. After all sectors, every `VertexController` updates.

An uncontrolled agent therefore moves during its sector update. A controlled agent's own update does nothing and the `VertexControllerArea` moves it later in the frame.

`Sector` stores `mCapacity` and exposes `getCapacity()`, but `enterAgent()` inserts unconditionally (`src/core/Sector.cpp:385-420`). No occupancy limit is checked.

### `Graph`, `Path`, `Vertex`, and `Edge`

`Pathing::findPath()` performs A*/Dijkstra-like route selection from edge weights. A `PathNode` contains an edge, target vertex, and edge weight. The first node is the source and has no edge.

An `Edge` advertises three execution-related operations:

- `isTraversable()` — whether crossing is currently legal.
- `requestTraversal()` — attempt to make crossing legal.
- `getWeight()` — estimated traversal time for route selection.

The comments in `src/core/Edge.cpp:24-51` describe this contract clearly. A repository-wide call search, however, finds only declarations and implementations of the first two operations; the agent executor does not call either one.

### `Agent`: individual behaviour

`Agent` has three states (`include/core/Agent.h:43-49`):

- `Idle`
- `MovingToVertex`
- `UnderVertexControl`

Normal execution is:

1. Walk in a straight line to the current target vertex using walking speed.
2. Increment the path-node index when the position is reached.
3. While approaching a controlled vertex, check whether the agent has entered its control area.
4. If the next transition is judged to require control, register with the vertex controller and stop self-updating.
5. The controller eventually calls `traversePathEdge(true)`, which atomically removes the agent from one sector, inserts it in the next sector, and skips the paired boundary vertex (`src/core/Agent.cpp:231-255`).

No continuous collision avoidance is present. `moveToPosition()` simply advances toward a point (`src/core/Agent.cpp:265-277`). This is compatible with the intended abstraction that agents passing in 2D may overlap, but waiting separation must be provided by explicit waiting positions.

`chooseVertexOffset()` is unfinished and always chooses entry zero (`src/core/Agent.cpp:279-290`).

### `VertexController`: threshold-level group behaviour

A `VertexController` owns up to two `VertexControllerArea` objects, normally one for each layer or side. It decides whether a path transition needs control and serialises exits.

The base `transitionRequiresControl()` only returns true when the next vertex changes layer or global cell Y (`src/core/VertexController.cpp:90-103`). This is a geometric heuristic, not a decision based on the edge's rules.

Each controller area contains:

- a control shape;
- optionally a device controller and its reachable area;
- optionally an exit area;
- reserved target offsets for the controller, exit, and queue positions;
- registered agents and their role.

Agent roles (`include/core/VertexControllerArea.h:20-28`) are:

```text
MoveToIntermediate → WaitInQueue → MoveToExit → WaitForExit → ReadyToExit
          └────────── UseController ──────────┘
```

The exact transition varies with door state and whether a button is needed:

- `UseController`: walk to and use a button.
- `MoveToIntermediate`: walk to a reserved queue position.
- `WaitInQueue`: remain at that position.
- `MoveToExit`: the controller has selected the agent; walk to the threshold.
- `WaitForExit`: request an unblock action, then wait for `canExit()`.
- `ReadyToExit`: cross and unregister.

`QueuedVertexControllerArea` generates one-dimensional X offsets at `CORE_DOOR_QUEUE_STOP_WIDTH`, which is agent width plus 0.1 units (`include/core/Defines.h:49`). This is the only intentional agent-spacing behaviour in the code.

### Doors

`DoorVertexController` is the most complete traversal controller:

- one area per fore/back layer;
- queue positions calculated from traversable cells beside the door;
- optional button per side;
- `canExit()` requires `Door::isOpen()`;
- a door sensor keeps the door open while an area reports an agent ready to exit.

A normal intended flow is:

```text
approach → register → optionally press button → queue
→ receive exit turn → wait for open door → cross → unregister
```

`BulkheadDoorVertexController` and `WindowVertexController` are scaffolding: both return `false` from `canExit()` and have empty action handlers. Bulkhead transitions also tend not to register because they are same-layer and same-Y.

### Device control: `Useable`, `Controller`, `Controllable`, and `Orchestrator`

- `Useable` gates whether an object can be used.
- `Controller` sends a device action to an `Orchestrator`.
- `Button` is both an `Object` and a `Controller`; pressing sends `Press` with the acting agent as subject.
- `Controllable` owns an asynchronous action list with callbacks.
- `Orchestrator` routes an action by the controller pointer to every registered `OrchestratedSystem`.

Examples:

- `ButtonDoorOrchestratedSystem`: button press → door open.
- `ButtonExtensibleObjectOrchestratedSystem`: button press → ladder/bridge toggle.
- `LiftOrchestratedSystem`: call button → lift call action → arrival callback → open stop door.
- `ShuttleOrchestratedSystem`: equivalent, potentially opening several car doors at a stop.

These classes coordinate devices, not agents or occupancy.

### Lift and shuttle state

`RailedTransport` supplies the vehicle state machine:

```text
Idle → Moving → Arrived → WaitingOpenAndDisembark
→ WaitingEmbarkAndClose → Leaving → Moving
```

A call is a `ControllableActionType::CallToStop`. On arrival its callback opens the stop door. Door callbacks inform the vehicle that doors opened or closed.

The state names mention embark/disembark, but there is no passenger manifest, no embark/disembark event from an agent, and no count of waiting or onboard agents. Time and door callbacks alone move the machine between these states.

Ladders and staircases are represented as transit sectors and specialised path edges, but they have no traversal controller or single-lane occupancy protocol.

## Intended invariants

The user-level rules imply the following invariants. They should be enforced at transition time, not merely represented as path weights.

1. **A path is not permission.** Every special edge must grant a traversal permit before sector membership changes.
2. **Door safety.** An agent crosses a door threshold only while that door is fully open.
3. **Controlled-door integrity.** If a button is required, the agent cannot bypass it by directly opening the door object.
4. **Capacity.** `occupants + committed entries <= capacity` for every finite-capacity resource.
5. **Vehicle presence.** Boarding and disembarking are legal only when the correct car is at the stop and its door is open.
6. **Disembark before embark.** At a stop, onboard agents leaving the resource get threshold access before waiting agents board.
7. **Departure barrier.** A vehicle cannot close/depart until crossing agents have completed and its departure policy is satisfied.
8. **Reservation ownership.** Every queue, threshold, and capacity reservation has exactly one owner and is released on completion, cancellation, or replanning.
9. **Contextual separation.** Waiting agents reserve separated positions; ordinary moving agents may pass through one another in the 2D abstraction.
10. **Fair progress.** If a resource remains operable, queued agents eventually receive service without one side starving.

## Confirmed logic gaps

### Critical: path execution bypasses edge rules

`Agent::moveToVertex()` walks toward every target without consulting the path edge. `traversePathEdge()` is only invoked by `VertexControllerArea` (`src/core/VertexControllerArea.cpp:429`). Consequently:

- `GapEdge::isTraversable() == false` is not enforced during execution.
- lift, shuttle, ladder, staircase, mount, force-bridge, and bulkhead edge conditions can be bypassed;
- the agent may walk globally to a vertex while remaining registered in its old sector;
- specialised climb speed is not used by the executor;
- capacity cannot be introduced reliably at the edge implementations because their execution contract is unused.

This is the primary seam to repair.

### Critical: lifts and shuttles have no rider model

Lift/shuttle transit sectors are constructed with capacity `~0u` (`src/core/LiftTransit.cpp:30`, `src/core/ShuttleTransit.cpp:30`). More importantly, there is no occupant collection or reservation API in `RailedTransport` or the transport orchestrated systems.

An agent crossing the stop door becomes a member of the entire transit sector, not a particular car. Car movement changes the `RailedTransport` object's position, but does not carry agent positions. Vertical/horizontal transport edges are always traversable and have minimum weight (`src/core/LiftEdge.cpp:44-56`, `src/core/ShuttleEdge.cpp:44-56`).

Therefore “car present,” “agent onboard,” “capacity available,” “disembarking complete,” and “safe to depart” are not representable as enforced facts.

### Critical: only area zero is allowed to exit

`VertexController::update()` checks and requests exits only from `mAreas[0]` (`src/core/VertexController.cpp:145-149`). Area one is updated but never selected for exit.

For a fore/back door, agents on the back side—including agents trying to leave a lift or shuttle—can queue and keep the door sensor blocked, but cannot be granted crossing permission. This directly explains one-sided embark/disembark failures.

### Critical: capacity is data only

`Sector::getCapacity()` is never consumed by movement logic. `Sector::enterAgent()` inserts without a check, reservation, or rejection path. Lift, shuttle, staircase, and ordinary locations use unlimited capacity; only ladder construction calculates a finite capacity, which is still unenforced.

A check inside `enterAgent()` alone would be insufficient because simultaneous entrants need reservations before they leave their queues.

### Critical: a controlled door can be opened directly

When an exiting agent reaches a closed door, `VertexControllerArea` calls `handleVertexAction(UnblockForExit)`. `DoorVertexController` responds with `door->handleAction(Open)` directly (`src/core/DoorVertexController.cpp:83-101`), regardless of whether the door requires a button or whether a lift/shuttle car is at that stop.

This bypasses the device orchestrator. At a lift stop it can open the landing door before the car arrives and then permit crossing because `canExit()` checks only `Door::isOpen()`.

### High: queue target selection can reserve the wrong target type

`getFreeIntermediateTargetOffsetIndex()` iterates over `mTargetOffsets[firstIntermediateIndex + i]` but stores `bestIndex = i` (`src/core/VertexControllerArea.cpp:218-236`). It should return the absolute target index.

When an area has an exit and/or controller target before its queue targets, a second or later agent can be assigned the controller or exit index instead of a queue index. The resulting agent may use a button unexpectedly, collide at the exit, or corrupt reservations.

### High: controller-user pre-emption corrupts reservations

In `getAgentRoleDetails()` (`src/core/VertexControllerArea.cpp:263-296`):

- the current `UseController` agent is demoted even when the arriving agent is not closer;
- in that branch the new agent's output role/index can remain uninitialised;
- the demoted agent's newly selected queue target is not marked occupied there;
- duplicate registration is not rejected.

The logic needs an explicit transfer operation that atomically releases and acquires both reservations.

### High: unregistering leaks target reservations

`unregisterAgentFromControl()` removes the registered-agent record but does not clear that agent's `mTargetOffsets[targetOffsetIndex].agent` (`src/core/VertexControllerArea.cpp:178-199`). The normal exit path happens to clear the exit target separately, but cancellation, replanning, deletion, or exceptional exits leave a ghost reservation.

There is also no cancellation protocol between `Agent::clearPath()` and a traversal controller.

### High: button use failures are ignored

On reaching a controller target, the area calls `mController->use(agent, {})` and immediately chooses a new role (`src/core/VertexControllerArea.cpp:402-406`). It does not branch on `Rejected`, `Unhandled`, or failure. A disabled or failed button can leave the agent queued without a pending request and without a retry policy.

Likewise, lift/shuttle door callbacks do not inspect callback status before reporting “door opened/closed” to the transport. `Controllable` also invokes callbacks for interrupted actions, so transport state can advance after an unsuccessful device operation.

### High: traversal-control detection is geometric and incomplete

`transitionRequiresControl()` checks only layer and Y changes. It misses same-layer horizontal bulkhead doors and cannot express rules such as “ladder is single lane,” “car must be present,” or “resource is full.”

Traversal control should be a property/policy of the edge or resource, not inferred from vertex geometry.

### High: transport direction mixes stop index and travelled distance

`mCurStop` is a travelled-distance value, while call data contains a stop index. `RailedTransport::startAction()` and `onDoorClosed()` compare `action.data.i - mCurStop` (`src/core/RailedTransport.cpp:308`, `:413`). This chooses the wrong direction for non-uniformly spaced stops after the vehicle has moved away from the origin. `getStopDistance(action.data.i)` should be used consistently.

`PlatformLift` repeats the same comparison (`src/core/PlatformLift.cpp:72`).

### Medium: queue advancement is asymmetric

`chooseNewAgentTarget()` takes the absolute value of the current offset and then uses it in a same-sign test against candidate offsets. Since the current value is no longer signed, negative-side queue positions do not advance consistently. The queue is also not explicitly FIFO; proximity and vector iteration order determine service.

### Medium: waiting separation is narrow and fragile

The intended waiting spread exists only as door X offsets. Target `size` is ignored, agents do not validate that their bounds fit a spot, and no interior lift/shuttle positions exist. `Agent::chooseVertexOffset()` always selects the first option.

This is not a reason to add global collision avoidance. A better fit for the simulation is discrete reservations for:

- door-side waiting positions;
- threshold crossing slots;
- lift/shuttle interior standing positions;
- single-lane ladder/stair slots where required.

Agents in ordinary `MovingToVertex` state can continue to pass through one another.

### Medium: edge weight defects can distort otherwise valid paths

Although route finding is reported as generally working, two formulas are reversed:

- `BulkheadDoorEdge` and `LadderEdge` calculate time as `speed / distance`, rather than `distance / speed` (`src/core/BulkheadDoorEdge.cpp:58`, `src/core/LadderEdge.cpp:59`).
- `BulkheadDoorEdge` adds opening time when the visible door **is open**, and omits it when visible and closed (`src/core/BulkheadDoorEdge.cpp:61`).

Lift and shuttle edges always cost the minimum and do not estimate waiting or ride time, so route choice cannot account for congestion or vehicle position.

### Medium: lift coordinate defects

- `liftStops()` ignores its `cellY` argument and creates stops at `y = offset` (`src/core/Lift.cpp:22-29`).
- `LiftTransit` computes vertical dimensions with `sectorOffsetX` instead of `sectorOffsetY` (`src/core/LiftTransit.cpp:27-28`).

These can desynchronise rendered car position, stop identity, and graph geometry, especially for lifts not based at Y zero.

### Medium: source-node lookup can dereference a null edge

The first path node deliberately has `edge == nullptr` (`src/core/Pathing.cpp:117`). `findNextVertexForVertexInPath()` dereferences `node.edge` starting at the supplied index (`src/core/Pathing.cpp:173-180`). `Agent::checkMovedUnderVertexControl()` can call it with target index zero. If the closest source vertex itself has a controller, this can dereference the null source edge.

### Incomplete controllers

`BulkheadDoorVertexController::canExit()` and `WindowVertexController::canExit()` always return false and their action handlers are empty. There are no traversal controllers for transport rides. These classes should not be considered production implementations of their represented rules.

## Recommended ownership model

The existing device-action framework can remain, but movement permission needs one authoritative layer.

### 1. Make every edge traversal explicit

Introduce a generic traversal transaction used by `Agent` for every edge:

```text
Approach source vertex
→ request permit from edge/resource policy
→ wait or reserve
→ prepare resource (press button/call/open)
→ receive permit
→ execute crossing/ride
→ commit sector/occupancy change
→ release permit
```

The transaction must be able to return at least: granted, waiting, rejected/replan, and cancelled.

### 2. Let resources own capacity and manifests

A finite-capacity resource should own:

- `capacity`;
- current occupants;
- committed entry reservations;
- pending exits;
- interior standing positions, where visual separation matters.

The invariant should be checked in one place:

```text
occupants + entryReservations <= capacity
```

Sector membership can mirror occupancy but should not be the sole source of truth for a multi-car shuttle.

### 3. Separate stop coordination from vehicle motion

A lift/shuttle stop coordinator should combine:

- landing door state;
- car identity and presence;
- waiting queues on the location side;
- disembark queue on the car side;
- a single threshold crossing reservation;
- departure barrier.

Suggested phase order:

```text
CarAbsent
→ ArrivedClosed
→ Opening
→ Disembarking
→ Embarking
→ Closing
→ Departed
```

Only the coordinator changes phase. Agents report completion of a granted crossing; timers should be fallback policy, not proof that crossings completed.

### 4. Keep collision handling contextual

Do not introduce expensive all-agent collision avoidance. Use discrete positional reservations only while an agent is:

- waiting at a threshold;
- occupying a constrained resource;
- crossing a single-file resource.

During ordinary room/corridor movement, overlap remains allowed by design.

### 5. Define cancellation and fairness

Every reservation API needs cancellation for path replacement, agent removal, device failure, and timeout. Queues should have a documented fairness policy—normally FIFO per side plus a resource-level policy for alternating sides or prioritising disembarkation.

## Suggested implementation order

1. Add a headless simulation test target; do not depend on SDL/ImGui.
2. Fix source-node safety and queue bookkeeping defects.
3. Change agent execution so every edge requests/receives a traversal permit.
4. Move door permission into that generic protocol; remove direct controlled-door opening.
5. Make both vertex-controller areas schedulable with an explicit fairness policy.
6. Add resource capacity reservations and cancellation.
7. Add lift/shuttle manifests, stop phases, and completion acknowledgements.
8. Attach rider positions to the current car while in transit.
9. Add ladder/stair single-lane policy where desired.
10. Correct transport coordinates, direction calculations, and edge weights.

## Minimum deterministic scenario suite

A headless harness should advance a building by fixed time steps and expose stable state snapshots. At minimum:

1. **Manual door, one agent:** agent waits for full open, crosses, and ends in the destination sector.
2. **Button door, failed press:** agent does not cross and either retries or replans.
3. **Two-sided door:** agents on both sides eventually cross; only one owns the threshold at once.
4. **Door closing safety:** an agent at/crossing the threshold prevents closure.
5. **Lift capacity:** with capacity two and three waiting agents, at most two board.
6. **Disembark priority:** riders leaving a full car exit before waiting agents board.
7. **Vehicle presence:** no agent crosses a landing door while the car is absent.
8. **Multiple lift calls:** non-uniform stops are served in the correct direction/order.
9. **Shuttle cars:** occupancy is associated with the correct car and door.
10. **Ladder contention:** opposing agents cannot occupy a configured single-file ladder simultaneously.
11. **Cancellation:** cancelling a queued agent releases all queue and capacity reservations.
12. **Waiting separation:** waiting-agent bounds do not overlap; unrelated moving agents may overlap.

These scenarios provide the feedback loop currently missing from the project and turn the invariants above into executable rules.
