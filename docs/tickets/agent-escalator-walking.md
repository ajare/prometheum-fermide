# Let Agents choose to walk on moving escalators

Status: agreed specification; not published to the issue tracker.

## Goal

Let Agents sometimes walk rather than stand on moving Escalators. Walking adds the Agent's regular effective walk speed to the moving steps' speed without changing the Escalator's permitted direction.

## Agreed behaviour

### Built-in Agent property

- Add **Escalator walking chance**, a built-in Agent property supplied through an Agent Tag.
- Its value is one finite scalar probability in `[0, 1]`, not a range from which an Agent samples another probability.
- The effective default is `0` when no assigned tag supplies the property.
- `0` always stands; `1` always walks.
- Follow the existing Agent tag property inheritance and conflict rules. Do not introduce tag precedence, combine probabilities, or use Agent groups for inheritance.
- Support authoring and inspection through the tag/property UI and the existing registry persistence and editing workflows. Reject out-of-range and non-finite values.
- Existing documents without this property retain standing-only behaviour.

### One decision per traversal

- Decide once when the Agent enters a moving Escalator for traversal, not when planning a Path, approaching, or waiting for admission.
- Retain the walk/stand decision for the remainder of that traversal. Do not reroll each tick or each speed query.
- A later traversal of the same Escalator makes a fresh decision; there is no permanent Agent–Escalator preference.
- Clear the transient decision when the traversal ends or is discarded. Existing cancellation, reset, and removal cleanup must not leave a stale decision for a later traversal.
- Admission and movement continue through the existing traversal protocol; the property grants no additional movement permissions.

### Movement speed

For the Escalator's permitted direction:

```text
standing speed = abs(escalator speed)
walking speed  = abs(escalator speed) + agent.getWalkSpeed()
```

- Use the Agent's effective regular walk speed, including its Walk speed modifier.
- Apply the rule to both upward and downward moving Escalators.
- Do not permit walking against the Escalator's movement direction.
- Stationary Staircases and other transit types remain unchanged.
- Do not add separate lanes, collision avoidance, or overtaking mechanics.

### Pathfinding

- Keep route-cost estimates based on standing speed, regardless of walking chance or an active traversal decision.
- Pathfinding and route-cost queries must not consume random draws or mutate traversal state.
- This first version intentionally does not predict the travel-time benefit of walking when selecting a Path.

### Determinism and persistence

- Use deterministic per-Agent simulation randomness, independent of Lua Agent behaviour randomness.
- Resetting and replaying identical authored inputs must reproduce the same sequence of walk/stand decisions.
- Rendering frame subdivision and observational queries must not affect the sequence.
- Persist the authored property through the Agent tag registry. Do not persist active walk/stand decisions or introduce live traversal/RNG checkpoint serialization.
- This is built-in simulation behaviour and works without an assigned Lua Agent behaviour.

### Diagnostics

- Expose the active Escalator walk/stand decision through read-only simulation diagnostics/snapshots.
- Distinguish an active standing decision from no active Escalator traversal.
- New animations, on-canvas indicators, and a dedicated visualization UI are out of scope.

## Implementation starting points

- `include/core/AgentTag.h` and the Agent tag registry implementation: typed property, validation, serialization, and existing property conflict conventions.
- `include/core/Agent.h` and `src/core/Agent.cpp`: effective property resolution, transient traversal decision, lifecycle cleanup, and deterministic per-Agent simulation state.
- `src/core/StaircaseEdge.cpp`: existing Escalator direction checks, standing-speed route cost, and traversal speed query. Keep cost queries observational and independent of the random decision.
- `include/core/Simulation.h` and simulation snapshot construction: read-only active decision diagnostics.
- Tag editing panels and headless smoke checks: authoring support and regression coverage.

Preserve ADR 0001's separation of route intent, device state, and traversal permission; ADR 0007's external Agent tag registry ownership; and ADR 0008's deterministic, isolated Lua runtime boundary. No Lua API expansion is required.

## Acceptance criteria

1. An Agent with no property, or chance `0`, always stands and moves at the existing Escalator speed.
2. Chance `1` always walks at step speed plus effective regular walk speed, including a non-default Walk speed modifier.
3. Upward and downward Escalators apply the same additive speed rule; reverse-direction traversal remains prohibited.
4. Stationary Staircases and other movement types retain existing behaviour.
5. An intermediate chance produces deterministic decisions once per entry. A controlled seeded test covers both outcomes and repeated visits without flaky statistical assertions.
6. Repeated ticks, traversal-speed queries, snapshot reads, and path searches never reroll an active decision. Path searches before entry do not change subsequent decisions.
7. Route-cost estimates remain standing-speed estimates for every probability.
8. Identical reset/replay runs reproduce decisions and movement outcomes, including runs using different render-frame subdivisions. Lua random draws do not alter Escalator decisions.
9. Completion, cancellation/discard, reset, and Agent removal leave no stale traversal decision.
10. Diagnostics distinguish walking, standing, and no active Escalator traversal.
11. Tag authoring, assignment, conflict validation, removal, save/load, and undo/redo follow existing property workflows; missing properties resolve to `0`.
12. Values outside `[0, 1]` and non-finite values are rejected; both endpoints round-trip correctly.

## Explicit exclusions

- Persistent preferences for particular Escalators.
- Per-Agent probability ranges or additional authored random samples.
- Walking-aware route optimization.
- Walking against the moving steps.
- Changes to queues, capacity, traversal permits, or device control.
- New crowd physics, lanes, overtaking rules, or animations.
- Publishing a GitHub issue or implementing this feature as part of writing the specification.
