# Separate device control from traversal coordination

Status: accepted

The simulation will replace the overlapping `Controller`, `Controllable`, `Orchestrator`, and `VertexController` hierarchies with explicit interaction points, typed device operations, and shared traversal resources. Edges describe route topology but traversal resources exclusively own queues, capacity reservations, and permits; `World` owns simulation entities and relationships use stable typed handles. This separation was chosen over extending the existing inheritance and callback model so that device state, movement permission, and object lifetime each have one authoritative owner.
