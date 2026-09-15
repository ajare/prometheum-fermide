# Building Movement Simulation

A simulation of people moving through a two-dimensional building while doors, lifts, shuttles, ladders, and other shared resources constrain when transitions may occur.

## World and routes

**Building**:
The complete simulated world, including its spatial structure, movement network, devices, and agents.

**Layer**:
One of the two overlapping spatial planes used to represent depth in the two-dimensional world: fore or back.

**Sector**:
An occupancy region to which an agent belongs at a point in time. A sector is either a stationary location or a transit region.

**Location**:
A stationary sector such as a room or corridor.
_Avoid_: Sector, when specifically referring to stationary space

**Room**:
A named location that may occupy either the fore or back layer.

**Corridor**:
A location that occupies the fore layer.

**Transit**:
A static sector that connects locations through one or more stops, such as a lift shaft, shuttle route, ladder, or staircase. Within a lift or shuttle transit, an agent may also occupy a specific transport vehicle or carriage.

**Stop**:
A place at which a transit resource connects to a location.

**Access zone**:
A connected waiting area at a stop whose usable doors share one logical boarding queue. Disconnected approaches are separate access zones.
_Avoid_: Stop, queue position

**Path**:
An agent's planned sequence of vertices and edges. A path expresses route intent, not permission to traverse every edge immediately.
_Avoid_: Reservation, traversal permit

**Transport journey**:
An agent's coordinated use of one transport vehicle, from waiting and boarding at an origin stop through disembarking at a destination stop.
_Avoid_: Path, ride edge

**Trip intent**:
An agent's desired origin and destination for a transport journey before or while the vehicle accepts the request.
_Avoid_: Stop request, reservation

**Stop request**:
An active demand for a transport vehicle to visit a stop, created by a landing call or an onboard destination selection.
_Avoid_: Trip intent, queue ticket

**Threshold**:
The controlled boundary between two sectors, such as a doorway or the entrance to a lift.
_Avoid_: Vertex, when discussing physical movement rules

**Walkway**:
A traversable floor within a multi-deck room, above that room's ground floor.

**Platform lift**:
An open transport vehicle within one room. Its ground stop is mandatory, and selected walkway stops connect it to higher levels in the same column. Walkways in that column need not all be stops.
_Avoid_: Lift, when distinguishing the open room object from an enclosed lift transit

## Actors and shared resources

**Agent**:
A simulated person with a position, destination path, and current movement state.

**Shared resource**:
A constrained facility that agents must coordinate to use, such as a lift car, shuttle carriage, ladder, staircase, or doorway.

**Transport vehicle**:
An independently scheduled moving resource: either a lift car or a complete coupled shuttle.
_Avoid_: Transit, transport car

**Carriage**:
A capacity-owning passenger compartment within a shuttle. All carriages in a shuttle move and are scheduled together.
_Avoid_: Vehicle, shuttle

**Capacity**:
The maximum number of agents that may occupy a shared resource at once.

**Occupant**:
An agent that has completed entry into a shared resource and consumes one unit of its capacity.
_Avoid_: Waiting agent, reserved agent

**Waiting agent**:
An agent that has requested a transition but has not yet received permission to enter or cross.
_Avoid_: Occupant

**Reservation**:
Exclusive permission for one agent to use a queue position, doorway crossing slot, or capacity slot.
_Avoid_: Path

**Queue ticket**:
An agent's logical priority in a waiting line, independent of where that agent is physically standing.
_Avoid_: Queue position, traversal permit

**Queue position**:
A reserved waiting position near a threshold. Queue positions keep waiting agents separated without imposing general collision avoidance on agents merely passing in two dimensions.
_Avoid_: Queue ticket, occupancy slot

## Interaction and coordination

**Door activation mode**:
The permitted way to open a door: automatically by presence, manually by the crossing agent, through a remote interaction point, or not at all.

**Interaction point**:
A place where an agent can request one or more typed device commands, usually represented by a physical button or switch.
_Avoid_: Device controller, traversal controller

**Device command**:
A typed request for a device state change, such as opening a door, extending a bridge, or calling a lift.
_Avoid_: Interaction, traversal request

**Device operation**:
The queryable progress and outcome of accepted device commands.
_Avoid_: Callback, traversal request

**Traversal resource**:
The authority that owns admission, queueing, capacity, reservations, and permits for movement through a shared resource or threshold.
_Avoid_: Traversal controller, device controller

**Traversal permit**:
A short-lived authorization to perform one specific sector transition after all door, capacity, and resource-position conditions are satisfied.
_Avoid_: Path, reservation
