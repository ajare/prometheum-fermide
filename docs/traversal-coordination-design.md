# Traversal Coordination Redesign

## Problem Statement

Agents can plan routes through the world, but executing those routes does not consistently enforce the rules represented by doors, lifts, shuttles, ladders, and other shared resources. Agents can bypass edge traversal checks, controlled doors can be opened without their controls, capacities are not enforced, transport passengers are not associated with a specific vehicle or carriage, and queue reservations can leak or be assigned incorrectly.

Device interaction and movement permission are split across overlapping controller hierarchies with ambiguous names and responsibilities. Callback-driven device actions, mixed pointer ownership, and update ordering make cancellation, failure, fairness, and safe transport departure difficult to reason about. The result is a system that can find the intended route but cannot reliably coordinate agents while they follow it.

## Solution

Replace the overlapping controller hierarchies with a single explicit traversal protocol and clearly separated responsibilities:

- Paths express route intent, never permission.
- Edges describe topology and cost while shared traversal resources own queues, capacity reservations, preparation, and traversal permits.
- Each agent executes one traversal task and remains responsible for its own movement.
- Interaction points accept physical interaction from agents and issue typed commands to devices or transport coordinators.
- Devices expose deterministic state machines and queryable, cancellable operations instead of callback chains.
- Transport vehicles own motion and scheduling; lift cars and shuttle carriages own occupants and interior positions; stop coordinators own door interlocks and boarding phases.
- The world owns simulation entities and advances them through fixed, deterministic phases.

Agents will approach resources, queue at reserved positions, operate required controls, wait for safe admission, cross visibly, and commit occupancy changes atomically. Lifts and shuttles will enforce capacity, destination selection, disembark-before-embark, door interlocks, dwell windows, and LOOK scheduling. The same resource model will support doors, ladders, narrow stairs, force bridges, windows, bulkheads, and platform lifts.

## User Stories

1. As an agent, I want every sector transition to request permission, so that my path cannot bypass a closed or unavailable resource.
2. As an agent, I want an ordinary passage to grant permission immediately, so that unconstrained movement remains inexpensive.
3. As an agent, I want to wait when a route is temporarily unavailable, so that a closed door or absent lift does not immediately invalidate my journey.
4. As an agent, I want to replan when another route becomes materially faster, so that I do not wait indefinitely for a poor route.
5. As an agent, I want hard rejection or disabled infrastructure to trigger replanning, so that I do not remain stuck on an impossible route.
6. As an agent, I want one active locomotion task, so that competing movement systems cannot control my position simultaneously.
7. As an agent, I want to move myself toward goals assigned by coordinators, so that all walking uses one consistent movement implementation.
8. As an agent, I want to cross thresholds visibly, so that movement does not teleport between sectors.
9. As an agent, I want my sector transfer to occur only after crossing completes, so that occupancy remains consistent during movement.
10. As an agent, I want assigned local goals to expire when I cannot reach them, so that bad reservations do not block everyone else.
11. As an agent, I want a failed local goal to be reassigned when possible, so that transient positioning problems do not immediately destroy my route.
12. As a player, I want manually directed agents to obey the same rules as autonomous agents, so that manual control cannot bypass simulation constraints.
13. As a player, I want explicit interactions to require physical reach, so that buttons cannot be activated remotely through normal gameplay.
14. As a developer, I want debug overrides to be explicit and separate, so that intentional cheats cannot be mistaken for normal traversal.
15. As an agent, I want to reserve a separated queue position, so that agents waiting near a threshold do not overlap.
16. As an agent, I want logical queue priority to be independent of my physical position, so that operating a control does not make me lose my place.
17. As an agent, I want to retain a logical queue ticket when nearby physical positions are full, so that I can wait upstream fairly.
18. As an agent, I want queue positions to advance as earlier positions become free, so that the line moves toward the threshold.
19. As an agent, I want queue order to be FIFO within my approach side, so that later arrivals do not overtake me.
20. As an agent, I want the oldest eligible side to receive the next door crossing, so that two-sided door traffic remains fair.
21. As an agent, I want wide openings to offer multiple crossing lanes, so that their physical width can improve throughput.
22. As an agent, I want queue priority to survive a compatible replan, so that changing a later part of my journey does not unfairly reset my wait.
23. As an agent, I want incompatible replans to release all old reservations, so that obsolete route state cannot block resources.
24. As an agent, I want one selected operator to activate a shared control, so that every waiting agent does not press the same button.
25. As an agent, I want an unnecessary interaction cancelled when its desired state is already achieved, so that I can continue without redundant movement.
26. As an agent, I want an active equivalent request to satisfy my need, so that shared device operations are reused safely.
27. As an agent, I want temporary interaction failures retried, so that a busy control does not permanently invalidate my route.
28. As an agent, I want permanent interaction rejection distinguished from temporary blocking, so that I can replan promptly when necessary.
29. As an agent, I want remote-controlled doors to require their designated controls, so that traversal cannot bypass the world’s control layout.
30. As an agent, I want automatic doors to open in response to coordinated presence, so that I can pass without pressing a control.
31. As an agent, I want manual doors to be operated directly at the threshold, so that their interaction matches their activation mode.
32. As an agent, I want unavailable doors to reject traversal, so that their state is respected by route execution.
33. As an agent crossing a door, I want it held fully open until I finish, so that it cannot close through me.
34. As a world occupant, I want ordinary doors to reopen for a new request or obstruction, so that closing doors remain safe.
35. As a world occupant, I want doors to close after their hold-open period, so that agents need not issue explicit close commands.
36. As a passenger, I want to call a lift or shuttle using its landing control, so that transport service requires physical interaction.
37. As a passenger, I want my intended destination known while I wait, so that only a compatible vehicle run admits me.
38. As a passenger, I want my pickup request activated only after the call control is used, so that merely approaching a stop does not bypass the button.
39. As a passenger, I want duplicate calls coalesced without losing individual ownership, so that one cancellation cannot remove another passenger’s service.
40. As a passenger, I want to keep my place when a vehicle is full, so that I receive service on a later run.
41. As a passenger, I want disembarking occupants to leave before new passengers board, so that capacity becomes available safely.
42. As a passenger, I want a carriage and door assigned based on walking distance and occupancy, so that boarding is distributed sensibly.
43. As a passenger, I want disconnected platform approaches to have separate queues, so that physically unrelated waiting lines are not merged.
44. As a passenger, I want doors on one connected access zone to share logical boarding priority, so that door assignment can balance capacity without overtaking.
45. As a passenger, I want both the landing door and vehicle door open before crossing, so that I cannot enter an absent or misaligned vehicle.
46. As a passenger, I want vehicle doors interlocked with alignment and motion, so that a vehicle cannot depart during a crossing.
47. As a passenger, I want destination selection to be an explicit interior interaction, so that my requested stop enters the vehicle schedule through a control.
48. As a passenger, I want an already-selected destination to satisfy my confirmation immediately, so that duplicate panel use is unnecessary.
49. As a passenger, I want new destination selections serialized at the interior selector, so that its single-user capacity is respected.
50. As a passenger, I want door closure to wait for destination confirmation, so that I am not carried without an active destination.
51. As a passenger, I want a failed destination interaction retried or safely cancelled, so that I am not trapped in an unscheduled journey.
52. As a passenger, I want my position anchored to my lift car or shuttle carriage, so that I move with the vehicle.
53. As a passenger, I want an interior standing position, so that onboard occupants remain visually separated.
54. As a passenger, I want the stop coordinator to assign a suitable disembark door, so that passengers do not converge on one threshold.
55. As a passenger, I want cancellation while moving to defer my exit until the next safe stop, so that I cannot leave a moving vehicle.
56. As a passenger, I want a replacement onboard route to submit a reachable new destination, so that every route change does not force an unnecessary exit.
57. As a passenger, I want transport doors to ignore late boarding calls after cutoff, so that the vehicle can depart.
58. As a passenger, I want transport doors to reopen for an obstruction or safety requirement, so that departure remains safe.
59. As a passenger, I want the vehicle to remain for a minimum dwell period, so that boarding and disembarking have time to complete.
60. As a passenger, I want a maximum boarding cutoff, so that continuing arrivals cannot keep a vehicle at one stop forever.
61. As a passenger, I want already-reserved boarders to finish after cutoff, so that accepted admission is honored.
62. As a passenger, I want lifts and linear shuttles to continue serving requests ahead before reversing, so that transport follows predictable LOOK scheduling.
63. As a waiting passenger, I want an idle vehicle dispatched toward the oldest outstanding request, so that distant stops cannot starve.
64. As a passenger, I want lift and shuttle occupancy enforced per physical car or carriage, so that declared capacity is never exceeded.
65. As a shuttle passenger, I want occupancy associated with my assigned carriage, so that a coupled shuttle’s separate capacities are respected.
66. As a platform-lift passenger, I want the same capacity and scheduling guarantees as an enclosed lift, so that the absence of doors does not remove movement safety.
67. As an agent using a ladder, I want opposing traffic prevented while I climb, so that a narrow ladder is not used in conflicting directions.
68. As an agent using a ladder, I want compatible climbers admitted up to its capacity, so that the ladder is not unnecessarily single-occupancy.
69. As an agent waiting for a ladder, I want directional batches bounded, so that continuous traffic from one side cannot starve me.
70. As an agent using an extensible ladder, I want it held extended while occupied or reserved, so that it cannot retract beneath me.
71. As an agent using an ordinary stairwell, I want unconstrained bidirectional movement, so that wide stairs do not create unnecessary queues.
72. As an agent using a narrow stairwell, I want configurable directional capacity, so that constrained stairs can use ladder-like coordination.
73. As an agent crossing a force bridge, I want it held extended while occupied or reserved, so that it cannot retract during use.
74. As an agent approaching a window, I want only a fully open window treated as normally traversable, so that closed or broken windows remain barriers.
75. As an agent using a bulkhead door, I want its queue and activation rules enforced until it is fully open, then unconstrained bidirectional passage with crossing safety leases, so that preparation cannot be bypassed and an open bulkhead does not serialize traffic.
76. As a level designer, I want vehicle capacity configured explicitly and validated against interior positions, so that declared capacity has a physical representation.
77. As a level designer, I want queue lanes configured by origin, direction, and extent, so that generated waiting positions are predictable.
78. As a level designer, I want invalid controls, queue lanes, doors, and interior positions rejected during construction, so that malformed worlds fail early.
79. As a level designer, I want per-resource timing overrides with sensible defaults, so that unusual devices do not require subclasses.
80. As a simulation operator, I want disabling a resource to preserve active safety, so that in-progress users reach a safe state.
81. As a simulation operator, I want disabled resources to reject new admissions and release pending requests, so that affected agents can replan.
82. As a developer, I want paths to estimate service and queue delays without reserving resources, so that route selection accounts for congestion without side effects.
83. As a developer, I want deterministic excessive-wait comparisons with hysteresis, so that agents do not oscillate between similar routes.
84. As a developer, I want explicit request, reservation, permit, lease, and operation ownership, so that cancellation releases exactly the state owned by one agent.
85. As a developer, I want one agent’s cancellation not to cancel a shared operation needed by others, so that deduplicated work remains valid.
86. As a developer, I want safety commands to override conflicting device commands, so that closing or retracting cannot be silently queued behind active traversal.
87. As a developer, I want typed operation results instead of enum-severity aggregation, so that failure handling remains meaningful.
88. As a developer, I want fixed simulation ticks and stable ID ordering, so that identical inputs produce identical outcomes.
89. As a developer, I want value-based events processed in defined phases, so that UI and logging cannot cause re-entrant simulation mutation.
90. As a developer, I want read-only coordination snapshots, so that queues, schedules, leases, and failures can be inspected without exposing mutable internals.
91. As a developer, I want the world to own simulation entities centrally, so that pointer cycles and dangling ownership relationships are eliminated.
92. As a developer, I want explicit names for interaction, device operation, and traversal concepts, so that device control cannot be confused with movement authority.
93. As a developer, I want one movement authority per edge during migration, so that legacy and replacement systems cannot grant conflicting permission.
94. As a developer, I want each migrated resource protected by deterministic behavioural tests, so that obsolete controllers can be removed safely.
95. As a developer, I want a headless simulation test target, so that movement correctness can be tested without graphics or audio dependencies.
96. As a developer, I want transport journeys to retain separate boarding, ride, and disembark graph edges, so that pathfinding can reason about stop topology.
97. As a developer, I want those edges executed as one transport journey, so that ownership and destination intent survive between stages.
98. As a developer, I want transport routes to remain static transit sectors, so that moving vehicles do not restructure the graph.
99. As a developer, I want exact vehicle and carriage occupancy tracked separately from transit-sector membership, so that multiple compartments remain distinguishable.
100. As a developer, I want the old controller hierarchy deleted after migration, so that there is only one supported coordination model.

## Implementation Decisions

### Architectural boundaries

- A controlled breaking redesign of the simulation core is permitted. Existing world construction, rendering, and path topology should be preserved where practical, but compatibility layers must not dictate the new model.
- Device interaction, device state, traversal permission, and movement are separate responsibilities.
- Ambiguous legacy base types will not be repurposed. The replacement vocabulary uses explicit concepts such as interaction point, device operation, traversal resource, traversal request, traversal permit, transport vehicle, carriage, and stop coordinator.
- An agent is an actor and does not inherit from a device controller or interaction target.
- Capabilities such as enabled state, interactability, command handling, sensing, and capacity ownership use composition and narrow interfaces rather than a shared usability hierarchy.
- The global pointer-based device-action router and per-device-combination orchestration classes will be replaced by explicit typed command bindings and resource coordinators.
- The accepted separation of device control from traversal coordination and the single-owner model is recorded as an architecture decision.

### Ownership and identity

- The world is the sole lifetime owner of agents, devices, traversal resources, vehicles, and coordinators.
- Relationships use stable typed IDs or validated non-owning handles rather than webs of owning shared pointers and raw back-pointers.
- Requests, reservations, permits, leases, operations, and stop requests have stable IDs and explicit owners.
- Removing or pausing an agent cancels all of that agent’s pending reservations and requests immediately. Occupancy already committed to a moving vehicle remains until a safe exit.
- Shared operations retain individual requester interests. Cancelling one requester removes only that interest and cancels the underlying work only when no dependants remain and cancellation is safe.

### Deterministic simulation

- Coordination remains single-threaded.
- Movement and coordination use a fixed simulation timestep independent of rendering.
- Each tick has explicit phases: advance device and vehicle state; collect agent intents; allocate reservations and permits; move agents; commit completed transitions; release state and publish events.
- Stable IDs provide deterministic tie-breaking wherever request age alone is insufficient.
- Devices own authoritative operation progress and completion. Rendering observes or interpolates simulation state and never completes an operation.
- State changes publish value-based events through a per-frame queue. Events support UI, logging, and secondary reactions but cannot cause re-entrant state mutation.

### Traversal protocol

- Every edge transition executes through the same protocol: request, wait or receive permit or rejection, execute, then commit or cancel.
- Ordinary unconstrained edges use the same protocol but grant immediately.
- Paths express route intent only and never allocate capacity or authorize crossing.
- Edges retain topology and route-cost responsibility while referring to an optional shared traversal resource for execution policy.
- Traversal resources exclusively own preparation requirements, logical queues, physical positions, admission reservations, crossing lanes, permits, and fairness.
- Each agent owns one active locomotion task. Queueing, control operation, crossing, boarding, riding, and disembarking are states or subordinate stages of that task.
- Coordinators assign movement goals but never mutate agent positions directly. Agents move themselves and report arrival or lack of progress.
- Threshold crossing is continuous movement between configured endpoints. The crossing permit and required safety leases remain active for the whole movement.
- Destination capacity is reserved before crossing starts. The agent remains a source occupant while crossing. At the destination endpoint, sector membership transfers atomically and the admission reservation becomes occupancy.
- Capacity and threshold permits are short-lived and expire on deterministic deadlines if the owner makes no progress.
- Queue tickets persist while route intent remains valid and do not expire merely because the wait is long.
- Reservation acquisition is staged: logical queue and physical position first, destination capacity only when actionable, and crossing lane immediately before movement.
- If an assigned position is unreachable, the agent releases short-lived reservations, retains compatible logical priority, requests another position, and eventually replans or fails after bounded retries.

### Route planning and replanning

- Route search never creates reservations or device operations.
- Edge costs include base traversal time and stable estimates for preparation, vehicle position, current schedule, and queue length.
- Temporarily unavailable resources remain valid route choices when service is expected.
- Hard rejection, disabled infrastructure, task cancellation, or repeated local-goal failure triggers replanning.
- After a minimum wait, an agent periodically compares the current estimated arrival time with alternatives. It replans only when an alternative is better by a configurable margin.
- Agents do not replan while holding an active admission or crossing permit.
- A compatible replan at the same immediate resource updates the existing request and retains queue priority. A change in origin, direction, or eligibility cancels the old request and creates a fresh queue entry.
- Required interaction points are not inserted as ordinary vertices into the main destination path. A traversal task creates a temporary route to the assigned control when preparation is required.
- An edge is unavailable when none of its applicable controls can be reached by an eligible operator.

### Queues, positions, and fairness

- Logical queue tickets and physical queue-position reservations are separate concepts.
- Queue lanes are configured with an origin, direction, and maximum extent. Discrete positions are generated and validated against walkable geometry.
- Agents reserve separated positions only in coordination contexts. Ordinary moving agents may overlap; global collision avoidance is out of scope.
- An agent selected to operate a control retains its logical queue ticket while temporarily releasing or changing physical reservations.
- Agents may join a logical queue when all physical positions are full but remain at upstream path positions until a slot is available.
- Door approaches use FIFO order within each side. The oldest eligible head across sides receives the next lane, with stable agent-ID tie-breaking.
- Wide thresholds may expose multiple discrete crossing lanes derived from or constrained by usable width. Ordinary doors default to one lane.
- All lanes for one threshold share door safety state and fairness policy.
- Transport boarding queues are maintained per stop, connected access zone, and travel direction.
- Multiple doors serving one connected access zone share a logical queue. Disconnected approaches retain separate queues, fairly arbitrated by the stop coordinator.
- Physical transport queue positions remain associated with specific doors, but reassignment between eligible doors does not alter logical priority.
- Transport disembarkation takes priority over boarding.
- No agent priority classes or group admission are included initially.

### Interactions and device operations

- An interaction point represents a physically usable control and accepts an explicit actor identity.
- Explicit AI or player interaction requires reachability and an interaction-position reservation. Remote activation is restricted to explicit debug or administrative commands.
- Physical controls default to one active user and a configurable interaction duration.
- A coordinator assigns at most one operator for a shared desired outcome, normally the earliest eligible agent able to reach an applicable control.
- Other agents retain their queue state and reuse the active request.
- If the desired state is achieved while an operator is approaching, the interaction task and control-position reservation are cancelled without losing queue priority.
- Commands are typed and express desired states such as open, extend, and call-to-stop. Traversal preparation never relies on a toggle command.
- UI toggles may remain but must resolve to an explicit desired-state command.
- Device commands return typed, queryable, cancellable operation handles instead of accepting arbitrary completion callbacks.
- Operation states include pending, running, succeeded, failed, and cancelled. Request outcomes distinguish temporary busy or blocked conditions from permanent rejection and execution failure.
- Temporary outcomes wait or retry with deterministic backoff. Execution failures have a bounded retry count. Permanent rejection triggers immediate cleanup and replanning.
- One interaction point may issue several commands. Bindings declare each command required or best-effort. Interaction succeeds only when every required command succeeds.
- Conflicting commands that violate an active safety requirement are rejected immediately rather than queued for later execution.

### Door coordination

- Doors declare one activation mode: automatic, manual, remote-controlled, or unavailable.
- Automatic doors translate coordinated presence into opening demand.
- Manual doors are operated directly by the crossing agent at the threshold.
- Remote-controlled doors require one of their designated controls and cannot be opened directly by traversal code.
- Unavailable doors reject traversal.
- Door-opening requirements use scoped leases. Active preparation, crossings, and transport stop phases may each own a lease.
- A door may close only when every open lease has been released and no obstruction is present.
- Ordinary doors wait a configurable hold-open duration after the final lease is released, then close automatically.
- Ordinary closing doors reopen for a new valid request or obstruction.
- Sensors publish presence and obstruction observations to coordinators rather than issuing door commands directly.
- Landing or platform doors and vehicle doors are distinct where both physically exist. Every required door must be fully open before crossing.
- A landing door cannot open unless the correct vehicle is aligned and stationary.
- A vehicle cannot move while any relevant door is open, opening, closing, obstructed, or protected by a crossing lease.
- A configuration with no physical door on one side may mark that side always open without weakening other interlocks.
- Bulkhead doors use the same preparation and safety protocol and are not exempt based on geometric direction. An automatic Bulkhead Door requests opening when any Agent in either adjacent Location is within its authored sensor distance, regardless of route intent. While closed or opening Bulkhead Doors queue normally; once fully open they release every waiter and grant concurrent bidirectional crossings without crossing-lane ownership.
- Fully open windows may act as thresholds. Closed, opening, closing, tinted, frosted, or broken windows are not normal traversal openings; hazardous broken-window traversal is deferred.

### Capacity and constrained resources

- Each agent consumes exactly one capacity slot.
- Finite resources enforce the invariant that committed occupants plus admission reservations never exceed capacity.
- Capacity ownership belongs to the physical shared resource, not merely to transit-sector membership.
- Lift and shuttle capacities are explicitly configured.
- Interior standing positions are generated and validated against vehicle geometry. Construction fails if declared capacity cannot be represented.
- Destination capacity remains reserved until entry completes or the reservation is cancelled or expires.
- Source occupancy remains committed until exit crossing completes.
- Ordinary locations and stairwells are unlimited by default.
- Narrow stairwells may opt into directional capacity.
- Ladders derive capacity from the number of crossed floors (`levelsHigh - 1`) divided by `CORE_LADDER_AGENT_SPACING / CORE_CELL_YX_RENDER_RATIO`.
- Ladders admit multiple same-direction climbers while preventing opposing occupancy and committed entry.
- Once opposite-direction demand exists, same-direction admissions stop after a configurable batch limit. Existing occupants drain before direction switches to the oldest waiting side.
- Extensible ladders hold extension leases while occupied, reserved, or being entered and cannot retract until every lease is released.
- Force bridges use equivalent extension leases and reject retraction while occupied, reserved, or actively being entered.

### Transport model

- A transit sector remains static and represents the shaft, route, ladder, or stairwell connectivity.
- Exact transport occupancy is represented separately by the vehicle manifest and spatial parent.
- One lift car is supported per shaft and one coupled shuttle per shuttle route in the initial implementation.
- A coupled shuttle is one scheduled and moving transport vehicle composed of capacity-owning carriages.
- Each carriage owns occupants, admission reservations, interior standing positions, and its physical doors.
- The transport vehicle owns motion, current direction, stop requests, and scheduling.
- A stop coordinator owns access-zone queues, vehicle alignment, door interlocks, disembarkation, boarding, and departure barriers.
- A passenger’s position is local to its lift car or shuttle carriage while onboard; global rendering position is derived from the vehicle transform.
- Boarding transfers the passenger from a location to the static transit sector and commits the passenger to the assigned vehicle or carriage.
- Disembarking transfers the passenger from the transit sector to the destination location only after crossing completes.
- Vehicle capacity and carriage manifests are authoritative; transit-sector membership alone is insufficient to determine occupancy.
- A graph path retains separate boarding, ride, and disembark edges. A contiguous group is executed as one transport journey so destination intent and resource ownership survive between stages.

### Transport calls and scheduling

- A waiting passenger registers a trip intent containing origin and destination.
- Registering trip intent does not itself dispatch the vehicle.
- A pickup stop request becomes active only after the assigned operator successfully uses the landing call control, unless an equivalent request is already active.
- Stop requests from landing calls and onboard destination selections are coalesced by stop while retaining every individual owner.
- Removing one owner does not remove a stop request still required by another owner.
- Lifts and linear shuttles use LOOK collective scheduling: continue in the current direction while requested stops remain ahead, then reverse when none remain ahead.
- Landing controls are initially non-directional, but waiting trip intents allow boarding eligibility to consider the passenger’s desired direction.
- When idle, a vehicle chooses the oldest outstanding request. Distance and stop ID are deterministic tie-breakers.
- Circular shuttle routes and fixed-direction scheduling are deferred.
- The stop coordinator assigns a boarding carriage and door by shortest walking distance and then least occupancy, with stable tie-breaking.
- The assignment reserves that carriage’s capacity and binds the passenger to it.
- If a vehicle fills, unadmitted passengers retain their queue tickets and stop-request ownership for a later service.
- At arrival, eligible onboard passengers receive disembark door assignments based on shortest interior route and current crossing load.
- All required disembarkation completes before boarding permits are issued.
- An open platform lift uses the same scheduling, capacity, dwell, and passenger-position rules but uses a virtual platform boundary instead of physical vehicle doors.
- Platform movement is prohibited while a boundary crossing is active.

### Destination selection and departure

- The stop coordinator knows waiting passengers’ destination intents before boarding.
- Passengers board only when their destination is compatible with the vehicle’s current run.
- After boarding, destination confirmation is an explicit part of boarding completion.
- Interior selectors are single-user interaction points, but passengers remain at their standing positions rather than walking to a modeled panel initially.
- New destination requests are submitted in deterministic boarding order and consume the configured interaction duration.
- If the destination already has an active stop request, the passenger becomes an owner and confirmation completes without redundant panel use.
- Vehicle doors do not close until all newly boarded passengers have confirmed destinations or resolved failures.
- A failed destination interaction retries within the boarding window. If it cannot succeed, the trip is cancelled and the passenger disembarks at the current stop.
- Stop service has a configurable minimum dwell duration.
- After minimum dwell, doors may close when no disembarker, active crossing, accepted boarder, or unresolved destination confirmation remains.
- A configurable maximum boarding cutoff stops new boarding reservations, preventing continuous arrivals from holding the vehicle forever.
- Passengers already holding valid admission reservations may finish after cutoff.
- A late boarding request after cutoff waits for the next service and does not reopen closing transport doors.
- An obstruction or new safety lease reopens transport doors.
- A route cancelled while a passenger is moving leaves the passenger onboard until the next safe stop. A compatible replacement route may submit a new reachable destination instead.

### Failure and deactivation

- Random breakdown simulation is out of scope, but deterministic disabling, rejection, interruption, timeout, and cancellation are supported from the start.
- Disabling a resource rejects new reservations immediately.
- Active threshold, bridge, and ladder safety leases remain valid long enough for in-progress crossings to finish.
- A moving vehicle reaches its next safe stop before becoming unavailable.
- Remaining pending requests are failed or released so agents can replan.
- Structural configuration errors are detected while building the graph and resources, not during agent traversal.
- Structural topology edits occur only while simulation ticks are paused. Affected tasks are cancelled, the relevant topology is rebuilt and validated, and simulation then resumes.

### Configuration, diagnostics, and scale

- Timings have resource-type defaults and optional per-instance overrides, including interaction duration, open and close duration, hold-open delay, dwell windows, retry delays, batch sizes, and permit deadlines.
- Invalid queue geometry, unreachable required controls, insufficient interior positions, mismatched transport doors, and invalid stop mappings reject world construction with precise diagnostics.
- Read-only diagnostic snapshots expose resource state, queues, positions, reservations, permits, leases, operations, manifests, and vehicle schedules.
- The target is hundreds of active agents and dozens of resources, with a stretch goal of approximately 1,000 agents.
- Coordination uses indexed local queues and resource-local work. Global all-agent collision detection and per-frame global replanning are prohibited.
- The existing MSVC and Visual Studio build remains in scope. The simulation core and headless test target must not depend on rendering, audio, or platform UI libraries.
- Save/load of active traversal state is deferred, but explicit ID-based state should remain snapshot-friendly.

### Migration

- Implementation proceeds as working vertical slices: establish a buildable baseline and deterministic headless harness; add the generic traversal transaction; migrate ordinary doors and queues; add generic capacity; migrate one enclosed lift; migrate shuttles; migrate ladders and stairs; migrate bulkheads, windows, force bridges, and platform lifts; then remove obsolete infrastructure.
- An edge has exactly one traversal authority during migration. Legacy vertex control and new traversal resources never coordinate the same edge simultaneously.
- Construction adapters may temporarily create either legacy or replacement resources, but no permanent compatibility layer remains in the core.
- After every resource type is migrated and protected by scenarios, remove the old usability, controller, controllable, orchestrator, orchestrated-system, and vertex-controller implementations outright.

## Testing Decisions

### Primary test seam

- The primary seam is the highest available boundary: construct a small headless world through the public simulation API, provide agent destinations or interaction intents, advance a fixed number of simulation ticks, and inspect public read-only state snapshots and emitted value events.
- Tests assert externally observable behaviour: agent sector and position, queue order, occupancy, active permits, door state, vehicle position, schedule, operation outcome, and eventual progress.
- Tests do not call private coordinator methods, mutate reservations directly, depend on container iteration order, or inspect owning pointers.
- This single scenario seam covers the integration among pathfinding, traversal tasks, interactions, devices, resources, and movement.

### Focused deterministic tests

- Focused unit tests are allowed for pure policies whose state-space is clearer below the full-world seam: LOOK scheduling, queue arbitration, directional batching, capacity accounting, and route-cost hysteresis.
- These tests use value inputs and outputs and do not mock internal object graphs.
- No automated simulation test suite currently exists, so there is no in-repository test prior art to preserve. Existing source-confirmed behaviour in the movement audit provides the baseline for regression scenarios.
- The headless test target must not link SDL, ImGui, rendering, or audio.

### Required scenarios

1. An ordinary unconstrained edge grants immediately and completes a sector transition.
2. A manual door requires the agent to reach and operate it before crossing.
3. An automatic door opens from coordinated presence and closes after its hold-open delay.
4. A remote-controlled door requires its configured control and cannot be opened directly by traversal code.
5. A failed or disabled control prevents crossing and causes bounded retry or replanning according to failure type.
6. Several agents waiting for one device produce one shared operation and one selected operator.
7. An operator retains logical queue priority while leaving its physical queue position.
8. A redundant interaction is cancelled when another request achieves the required state.
9. Two-sided door queues eventually serve both sides in deterministic FIFO order.
10. A wide threshold permits no more crossings than its configured lanes.
11. A closing ordinary door reopens for a new request.
12. An active crossing or obstruction prevents closure.
13. Cancelling or removing a queued agent releases every position, reservation, permit, and requester interest it owns.
14. Expired admission and crossing permits are reassigned without losing unrelated queue state.
15. An unreachable local goal is retried and eventually replanned without leaving ghost reservations.
16. A full physical queue lane retains overflow agents upstream in logical order.
17. Excessive-wait replanning uses ETA comparison and hysteresis rather than a universal timeout.
18. Pathfinding accounts for stable preparation and waiting estimates without creating reservations.
19. A lift remains absent until called through the landing interaction.
20. A landing door cannot open while its lift car is absent or misaligned.
21. Lift boarding never exceeds configured capacity, including simultaneous reservations.
22. A full lift leaves unadmitted passengers queued for a later run.
23. Lift occupants disembark before boarding begins.
24. An onboard passenger remains attached to the moving lift car and transit sector.
25. Every newly boarded passenger confirms a destination before door closure.
26. Duplicate destination selections share one stop request and avoid redundant selector use.
27. A failed destination selection retries, then lets the passenger leave at the current stop if unresolved.
28. Lift LOOK scheduling serves requests ahead, reverses correctly, and uses deterministic idle dispatch.
29. Non-uniformly spaced stops do not affect direction selection incorrectly.
30. A late passenger after boarding cutoff does not reopen transport doors.
31. An obstruction or active safety lease does reopen transport doors.
32. A cancelled onboard journey exits at the next safe stop or updates to a compatible new destination.
33. A shuttle assigns passengers to valid carriages without exceeding any carriage capacity.
34. Several shuttle doors share access-zone priority while disconnected platforms remain separate.
35. Shuttle disembark door assignments balance crossings deterministically.
36. Shuttle LOOK scheduling coalesces duplicate stop requests while preserving requester ownership.
37. An open platform lift cannot move during a boundary crossing.
38. Same-direction ladder users enter up to capacity while opposite-direction users wait.
39. Opposite ladder traffic receives service after the current bounded batch drains.
40. An occupied or reserved extensible ladder cannot retract.
41. An ordinary stairwell remains unconstrained and bidirectional.
42. A configured narrow stairwell enforces directional capacity.
43. An occupied or reserved force bridge cannot retract.
44. A fully open window permits configured traversal while closed or broken windows do not.
45. Same-layer bulkhead traversal queues until the door is fully open, then admits concurrent bidirectional crossings without queue or crossing-lane serialization.
46. Disabling an active crossing resource allows safe completion but rejects new admission.
47. Disabling a moving vehicle lets it reach a safe stop, then releases pending requests for replanning.
48. Invalid queue geometry, capacity geometry, control reachability, and transport-door mappings fail construction with useful diagnostics.
49. Repeated runs with identical inputs produce identical event, queue, schedule, and final-state snapshots.
50. Player-directed agents obey the same queue, capacity, and traversal rules as autonomous agents.

## Out of Scope

- Random device or vehicle breakdown simulation.
- Variable capacity consumption by different agent sizes; every agent consumes one slot.
- General all-agent collision avoidance in rooms and corridors.
- Agent priority classes, emergency queue bypass, or player queue privilege.
- Atomic group travel or all-or-nothing group boarding.
- Circular shuttle routes and fixed-direction loop scheduling.
- Multiple independently moving vehicles in one lift shaft or shuttle route.
- Lift-bank dispatch across several independent shafts.
- Physical walking and spatial queues around interior destination panels; selection is logically serialized while passengers remain at standing positions.
- Hazardous traversal through broken windows.
- Runtime structural topology mutation during active simulation ticks.
- Save/load serialization of active traversal tasks, reservations, and schedules.
- Cross-platform build-system migration.
- Global power consumption, power-grid simulation, or random maintenance failures.
- Retaining deprecated compatibility aliases for the old controller hierarchy after migration.

## Further Notes

- The domain glossary is authoritative for terms including path, transit, threshold, shared resource, occupant, waiting agent, reservation, queue ticket, queue position, device controller, traversal controller, traversal permit, transport vehicle, carriage, transport journey, trip intent, stop request, and access zone.
- The accepted architecture decision requires device state, traversal permission, and object lifetime to have separate authoritative owners.
- The current source contains partially overlapping and apparently incomplete interaction abstractions. The first implementation slice must restore a coherent buildable baseline before adding replacement behaviour.
- The current movement audit identifies source-confirmed defects including bypassed edge rules, one-sided threshold scheduling, unenforced capacity, direct controlled-door opening, reservation leaks, ignored action failures, incomplete controllers, transport direction defects, and coordinate defects. Migration should fix those behaviours through the new ownership model rather than patching each legacy class independently.
