# Agent movement and coordination

## Scope and status

This document describes the current movement and coordination model after the traversal-resource migration. The legacy usability, controller, orchestrator, and vertex-controller hierarchies have been removed. Deterministic headless scenarios cover ordinary movement, controlled thresholds, extensible resources, lifts, shuttles, cancellation, failure, topology rebuilds, and typed interaction.

## Authoritative model

```text
World
├── Entity registries (stable typed IDs)
│   ├── Agent
│   ├── InteractionPoint / InteractionRequest
│   ├── DeviceOperation
│   └── TraversalResource / TraversalRequest / TraversalPermit
├── Layer
│   └── Sector (Location or Transit)
└── Graph
    ├── Vertex
    └── Edge (route topology + TraversalResourceId or immediate permit)
```

Responsibilities are deliberately separate:

- The graph describes where an agent can plan to go.
- An interaction point accepts an agent request and emits typed device commands.
- A device operation exposes the progress and result of a state change.
- A traversal resource owns admission, queues, capacity, reservations, leases, and permits.
- A traversal permit authorizes exactly one committed sector transition.
- `World` owns the registries, advances the protocol, and performs transfers.

Physical buttons are renderable state only. Each actionable button records the stable `InteractionPointId` for the world-owned interaction point that supplies its behaviour.

## Fixed-tick execution

`World::advanceTick()` runs six ordered phases:

1. **Resource advancement** — advance doors, extensible devices, lifts, shuttles, and device operations.
2. **Intent collection** — collect interaction and traversal demand.
3. **Allocation** — assign queue positions, capacity, leases, operations, and permits deterministically.
4. **Movement** — move agents only within the authority granted for the tick.
5. **Commit** — perform authorized sector transitions.
6. **Cleanup and event publication** — expire or release ownership and publish stable snapshots/events.

Elapsed render time is accumulated into whole fixed ticks. Simulation decisions therefore do not depend on render-frame subdivision.

## Paths and permission

A path is route intent, not movement authority. Before crossing an edge, an agent submits a traversal request to the edge's traversal resource. Edges that need no shared coordination use the explicit immediate-permit policy; all other edges identify one resource.

A granted permit is short-lived and belongs to one request and one agent. Crossing occurs only in the commit phase while that permit is live. Cancellation, timeout, disablement, replanning, and entity removal release associated queue positions, reservations, leases, and permits. Each read-only traversal-request snapshot includes a stable diagnostic that explains its current wait, active permit, denial, cancellation, or completed commit without requiring a UI to infer protocol internals.

## Controlled thresholds

Door, bulkhead, and window-threshold resources own their crossing queues. Door resources also coordinate preparation with the physical door and its activation mode:

- **Automatic** resources request opening from presence.
- **Manual** resources use an interaction point reachable by the crossing agent.
- **Remote controlled** resources require a configured physical interaction point.
- **Unavailable** resources reject admission.

A crossing permit is not granted until the threshold is physically safe. Crossing leases and sensor observations prevent closure while an agent occupies the threshold. Wide doors expose deterministic lanes; queues from both sides retain stable, fair ordering.

## Extensible resources

Ladders and force bridges use desired-state device operations and independently owned extension leases. Admission closes immediately when safe retraction is requested, but physical retraction waits for request and occupant leases to drain.

Finite-capacity and directional ladders serialize incompatible demand. Ladder movement uses configured climb speed rather than ordinary horizontal walking speed. Stairwell coordination remains explicit and opt-in. See [Sector Ladder agent flow](sector-ladder-agent-flow.md) for the endpoint, queue, admission, climb, and exit process with code references.

## Lifts and platform lifts

A transport journey tracks call, origin, destination, reservation, boarding, onboard occupancy, and disembarkation. Lift resources own capacity and deterministic stop scheduling. Stops move through explicit phases so disembarkation precedes boarding and doors cannot conflict with vehicle motion. An idle car services admission demand at its currently aligned stop before dispatching empty to a remote call, including callers whose physical call interaction is still in progress.

Passengers are attached to the moving vehicle while onboard. Each Platform Lift has one per-instance stop duration, defaulting to 10 seconds. It leaves on that fixed schedule: waiting callers neither shorten nor extend the stop. Each platform stop has a physical waiting lane, and callers retain logical ticket priority after operating the landing control. Boarding is an instantaneous transfer from the queue head to a fully-inside platform threshold, after which ordinary locomotion continues toward the reserved onboard position while vehicle motion is applied independently. A caller that misses the cutoff remains queued in place until the car returns; unrelated passing Agents never affect the schedule. Path following applies the general skippable-vertex rule on final exit: after looking through coincident topology-only nodes, an intermediate waypoint is skipped only when it and the following physical vertex share the agent's layer and height, lie on opposite horizontal sides of the agent, and require no action at the intermediate waypoint. Contiguous Platform Lift graph legs form one transport journey to the final requested stop; intermediate graph levels do not create stop demand, and the shared LOOK scheduler passes them unless another caller or passenger requested them.

Cancellation, disablement, and device failure drain ownership and direct passengers to a safe exit or terminal failure state rather than leaving reservations behind.

## Shuttles and carriages

A shuttle is one scheduled vehicle that may contain multiple capacity-owning carriages. Carriages move together but maintain independent occupancy and door state. Access zones separate disconnected waiting approaches, and a boarding assignment binds an agent to a specific carriage.

Within each carriage, passengers retain boarding order from the leading to the trailing end of the current run. One passenger walks as far toward the leading end as the side buffer allows; two or more passengers spread evenly across the buffered usable width. A pending boarder projects the post-boarding layout first, and its crossing waits while existing passengers walk to any adjusted targets needed to leave buffered entry space. Targets never teleport passengers, and vehicle translation remains independent from their carriage-relative locomotion. Capacity validation reserves 0.1 units between an Agent's maximum-width bounds and both carriage sides and neighbouring Agents. At the destination, the ride completes at the passenger's current buffered position and disembarkation selects the nearest Door serving that carriage and destination access sector.

Stop demand is coalesced physically while requester ownership remains independent. Cancelling one requester does not cancel another requester's valid demand.

## Interaction and device operations

Interaction is requested with `InteractionPointId` and `AgentId`. Bindings contain typed `DeviceCommand` values and declare whether each command is required or optional. Requests may share a compatible physical operation while retaining independent requester ownership.

Callers observe immutable snapshots and operation states (`Pending`, `Running`, and terminal outcomes) instead of receiving device-completion callbacks. UI interaction resolves the stable ID from a physical button and submits the request through `World`; rendering reads physical state without mutating simulation objects.

## Ownership and topology

`World` is the authoritative lifetime owner for migrated entities. Relationships use typed IDs rather than owning or ambiguous raw pointers. Removing an entity invalidates its handle and cleans dependent ownership.

Structural graph edits are allowed while paused. Rebuild validates that each edge has exactly one authority or the immediate-permit policy, swaps topology atomically, restores valid path intent, and cleans requests and permits that referred to retired resources.

## Behavioural coverage

The headless suite verifies, among other cases:

- ordered phases and deterministic snapshots/events, including repeated 500-agent runs;
- player-directed paths entering the same request/permit protocol as autonomous paths;
- a measured 1,000-agent stretch run that reports elapsed time and working-set memory;
- ordinary, denied, cancelled, and expired traversal;
- typed interaction aggregation and requester-local cancellation;
- automatic, manual, remote-controlled, and unavailable doors;
- fair two-sided queues, wide-door lanes, and safe closure;
- finite-capacity, directional, and extensible ladders;
- coordinated force bridges and opt-in stairwells;
- platform lifts, multi-stop lift scheduling, and failure recovery;
- single- and multi-carriage shuttle journeys and access zones;
- atomic paused topology rebuild and ownership cleanup.
