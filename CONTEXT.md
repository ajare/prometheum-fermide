# Building Movement Simulation

A simulation of people moving through a two-dimensional building while doors, lifts, shuttles, ladders, and other shared resources constrain when transitions may occur.

## World and routes

**Building**:
The complete simulated world, including its spatial structure, movement network, devices, and agents.

**Layer**:
One of the ordered spatial planes used to represent depth in the two-dimensional world. Layers are numbered from front (0) to back. A Building may have between 2 and 256 layers.

**Sector**:
A region of a Layer in the world's spatial structure. A sector is either a Location, a Transit, or a Background; an agent belongs to a Location or a Transit, never to a Background.

**Location**:
A stationary sector such as a room, corridor, or façade.
_Avoid_: Sector, when specifically referring to stationary space

**Room**:
A named location that may occupy any layer.

**Corridor**:
A location that may occupy any layer.

**Facade**:
A location whose perimeter walls are all open. It is occupiable, hosts every object type a Room hosts, owns walkable floor, and takes part in traversal exactly as a Room does; its only differences are that every wall end on every deck is open, and that it is rendered as a solid opaque colour like a Background. The open perimeter is an intrinsic creation property, not an editable state: wall add/remove commands and Bulkhead Doors refuse a Facade.

**Background**:
A non-occupiable Sector that exists only to be seen through Windows and other apertures from the Layer in front. It carries one opaque colour, hosts no objects, owns no walkable floor, and takes no part in traversal. Where a Window looks into a sector, that sector is its Background - no separate domain noun is minted for it.

**Transit**:
A static sector that connects locations through one or more stops, such as a lift shaft, shuttle route, ladder, or stairwell. A transit is placed on layer L and its landing locations are on layer L-1. Within a lift or shuttle transit, an agent may also occupy a specific transport vehicle or carriage.

**Stairwell**:
A compact stair transit that may connect several consecutive levels using alternating flights.
_Avoid_: Staircase

**Staircase**:
A single straight flight of steps connecting two adjacent levels. It may rise in either horizontal direction and is bidirectional while stationary.
_Avoid_: Stairwell

**Escalator**:
A staircase whose steps move at a non-zero speed. Its movement direction determines its sole permitted travel direction and its agents' travel speed.

**Stop**:
A place at which a transit resource connects to a location.

**Access zone**:
A connected waiting area at a stop whose usable doors share one logical boarding queue. Disconnected approaches are separate access zones.
_Avoid_: Stop, queue position

**Path**:
An agent's planned sequence of vertices and edges. A path expresses route intent, not permission to traverse every edge immediately.
_Avoid_: Reservation, traversal permit

**Marker**:
A named, Building-owned authored point in a Location with stable identity. A Marker may be selected as an Agent behaviour's destination, and its identity survives rename. Agent behaviours cannot choose arbitrary Vertices as destinations.
_Avoid_: Vertex, destination vertex

**Route loss**:
The condition in which an Agent's selected destination has no valid Path, either when movement first begins or after the simulation attempts to replace an invalidated Path. An Agent behaviour may respond by choosing a new destination.
_Avoid_: Replan, which recalculates a Path to the same destination

**Skippable path vertex**:
An intermediate waypoint that an agent need not physically visit when the agent and the next two physical vertices share a layer, those vertices lie horizontally on opposite sides of the agent at the same height, and no interaction or other specific action is required at the nearer vertex. Coincident topology-only vertices do not count as distinct physical waypoints.
_Avoid_: Removing the vertex from the authored path

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

**Door**:
A threshold that connects two adjacent layers. It is authored on the front layer of the pair and opens into the layer immediately behind it. A regular Door has a one-deck footprint. An ordinary Door authored in a Room may be regular height or 0.9-unit tall; Corridor, Facade, Lift, and Shuttle Doors always use regular height. Lift and Shuttle landing doors belong to their transport.
_Avoid_: Portal

**Crossing width**:
At a Door, the symmetric distance either side of the threshold vertex's x position within which an agent on the threshold row may begin crossing. Derived from the physical doorway width (cell width minus the door's x insets) minus the agent's width; a 1-cell door yields +/-0.2. The width relaxes where a crossing starts, never who crosses: queue order, lane allocation, and safety interlocks are unchanged, and the band never stops an agent.
_Avoid_: Door width, doorway width, when referring to the arrival band

**Window**:
A threshold, similar to a Door, that connects two adjacent layers and is authored on the front layer of the pair. It always looks into the layer directly behind it, so it can never sit on the back-most layer.

**Walkway**:
A traversable floor within a multi-deck room, above that room's ground floor.

**Platform lift**:
An open transport vehicle within one room. Its ground stop is mandatory, and selected walkway stops connect it to higher levels in the same column. Walkways in that column need not all be stops.
_Avoid_: Lift, when distinguishing the open room object from an enclosed lift transit

## Actors and shared resources

**Agent**:
A simulated person with a position, destination path, and current movement state.

**Agent behaviour**:
A reusable state-machine definition that may direct many Agents. Each assigned Agent runs an independent instance of the behaviour.
_Avoid_: Agent group, Agent tag

**Agent behaviour configuration**:
Authored data supplied to one Agent's behaviour instance according to its Agent behaviour schema. Agents using the same Agent behaviour may have different configurations, such as different schedules.
_Avoid_: Behaviour state, which is the instance's changing runtime state

**Agent behaviour schema**:
The named, typed fields an Agent behaviour requires in each Agent behaviour configuration. A Marker field is selected by name but retains the Marker's stable identity.
_Avoid_: Behaviour configuration, behaviour state

**Agent behaviour registry**:
A collection of reusable Agent behaviours that forms one shared behaviour namespace for the Buildings that reference it.
_Avoid_: Agent tag registry, script directory

**Agent tag**:
A named reusable set of Agent properties that may be assigned to many Agents.
_Avoid_: Agent group, label

**Agent property**:
One typed appearance or behaviour value supplied by an Agent tag.
_Avoid_: Agent attribute, when referring to a value supplied by a tag

**Agent tag registry**:
A collection of Agent tags that forms one shared tag namespace for the Buildings that reference it.
_Avoid_: Tag list, Agent group registry

**Agent tag assignment**:
An association between one Agent and one Agent tag.
_Avoid_: Agent group membership

**Agent group**:
A named, Building-scoped classification that may be assigned to an Agent for administrative organisation. An Agent group retains its identity when renamed; deleting it leaves its Agents with no Agent group. It does not confer inherited simulation behaviour, but may be used to activate or deactivate all its current Agents at once; each Agent retains its own activation and may be changed individually afterwards.
_Avoid_: Group, when the broader term could be ambiguous

**Shared resource**:
A constrained facility that agents must coordinate to use, such as a lift car, shuttle carriage, ladder, stairwell, or doorway.

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

**Door opening style**:
The visual manner in which a Door's leaf or leaves reveal its threshold: OpenUp, OpenLeft, OpenRight, or OpenApart. Opening style does not change the Door's state, obstruction, or traversal behaviour. A tall OpenUp Door takes proportionally longer to open and close so its leaf moves at the same vertical speed as a regular Door.

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
