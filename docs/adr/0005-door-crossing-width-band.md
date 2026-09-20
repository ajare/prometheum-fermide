# Door vertices carry a crossing-width band for arrival and lane grants

Status: accepted

Crossing a Door previously required an agent to converge on the exact threshold
position: the request gate fired on arrival at the Door vertex, and the lane
grant's head-of-queue check required the agent to stand exactly at its assigned
queue position (`distanceTo(position) <= 0.001`). For wide doors this forced
agents to walk to the door centre even when the doorway beside it was free,
and made grant timing depend on exact float convergence.

Door vertices now carry a **crossing width**: a symmetric half-width about
the vertex's x position, derived from the physical doorway and never
serialized:

```
crossingWidth = (cellsWide - 2 * CORE_DOOR_X_INSET - CORE_AGENT_MAX_WIDTH) / 2
```

A 1-cell door yields +/-0.2; a 3-cell door +/-1.2. The lane grant's
head-of-queue arrival check (`SimulationCoordinator::tryGrantDoorQueue`)
becomes the band predicate - `|agent.x - threshold.x| <= crossingWidth` and
`|agent.y - threshold.y| <= 0.001` - instead of arrival at the exact
assigned position. A waiting head of queue is granted the moment it stands
anywhere inside the band at the threshold row, and the scripted in-place
crossing (ADR predecessor work in #96) executes from where the agent is.

The width relaxes *where* a crossing starts, never *who* crosses. Queue
tickets, FIFO order per side, oldest-eligible-head-across-sides selection,
agent-ID tie-breaking, crossing lanes as geometry-free concurrency counters,
queue position assignment, permits, and door safety leases and interlocks are
all unchanged. The band never stops an agent: a waiting agent still aims at
its assigned queue position and the grant interrupts the walk.
`stopForAvailableQueuePosition` is untouched.

## Considered options

- **Keep the exact-centre arrival check and widen it with ad-hoc tolerances at
  each call site.** Rejected: the same predicate serves both the grant gate and
  (ticket #98) the request-creation gate; one named band keeps them identical.
- **Lane sub-intervals (geometry per crossing lane).** Rejected: crossing
  lanes are concurrency counters, not spatial partitions; binding geometry to
  lanes would couple queue geometry to lane count and break the
  geometry-free counter model.
- **Store the crossing width as serialized door state.** Rejected: it is a
  pure derivation from cell width, the door's x inset, and the agent width;
  serializing it would create a second source of truth.
- **Apply the band to lift/shuttle landing doorways and bulkhead doors too.**
  Rejected for now: those thresholds have vehicle-alignment interlocks that
  keep them centre-based; the band applies to plain Door vertices only.
  Force Bridges keep their exact-arrival grant check.

## Consequences

- `DoorVertex::getCrossingWidth()` exposes the derived half-width;
  `CORE_DOOR_CROSSING_HALF_WIDTH(cellsWide)` in `Defines.h` is the single
  formula, and `isWithinDoorCrossingBand` in `Coordination.h` is the single
  predicate.
- Grants can now land while the grantee is mid-stride inside the doorway.
  This is only safe because crossing execution is in-place (#96); the
  far-side Door vertex is never a movement target.
- A 1-cell door keeps its old behaviour in practice, differing only by the
  +/-0.2 arrival tolerance.
- The initial request-creation gate (`Agent::moveToVertex`) adopts the same
  band in ticket #98; this ticket changes only the grant gate.
