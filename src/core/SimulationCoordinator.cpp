#include "core/SimulationCoordinator.h"

#include "core/World.h"


namespace core
{

	// ADR 0004 moves simulation behaviour in leaf-first increments, splitting
	// the implementation per seam across further SimulationCoordinator*.cpp
	// translation units behind this single header so no translation unit
	// recreates the size problem the refactor removes. Agent lifecycle lives in
	// SimulationCoordinatorAgents.cpp, interactions and device operations in
	// SimulationCoordinatorInteractions.cpp, door and extensible traversal
	// preparation in SimulationCoordinatorDoors.cpp, the lift scheduling helpers
	// and passenger safe exits in SimulationCoordinatorLifts.cpp - which also
	// holds the lift allocation dispatcher, the lift boarding branch, the lift
	// riding branch and the disembarking branch - the open platform lift
	// traversal allocation in SimulationCoordinatorPlatformLifts.cpp, the
	// shuttle door assignment in SimulationCoordinatorShuttles.cpp, the queue
	// tickets, queue positions, door queue, traversal progress and permit expiry
	// in SimulationCoordinatorQueues.cpp, the ladder admission family in
	// SimulationCoordinatorAdmissions.cpp, and the traversal transaction
	// lifecycle in SimulationCoordinatorTraversal.cpp, the tick pipeline - the
	// fixed timestep, the simulation phases, lift and door resource advancement
	// and tick event publication - in SimulationCoordinatorTick.cpp, and every
	// snapshot builder in SimulationCoordinatorSnapshots.cpp. Nothing but
	// world-structure editing and forwarding remains in World.
	SimulationCoordinator::SimulationCoordinator(World& world)
		: mWorld(world)
	{
	}

} // core
