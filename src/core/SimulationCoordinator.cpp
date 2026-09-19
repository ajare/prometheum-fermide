#include "core/SimulationCoordinator.h"

#include "core/Building.h"


namespace core
{

	// ADR 0004 moves simulation behaviour in leaf-first increments, splitting
	// the implementation per seam across further SimulationCoordinator*.cpp
	// translation units behind this single header so no translation unit
	// recreates the size problem the refactor removes. Agent lifecycle lives in
	// SimulationCoordinatorAgents.cpp, interactions and device operations in
	// SimulationCoordinatorInteractions.cpp, door and extensible traversal
	// preparation in SimulationCoordinatorDoors.cpp, and the lift scheduling
	// helpers and passenger safe exits in SimulationCoordinatorLifts.cpp; the
	// remaining seam families - the shuttle door assignment, the lift allocation
	// branches, the queue and admission core, and the tick pipeline - follow in
	// later stages.
	SimulationCoordinator::SimulationCoordinator(Building& building)
		: mBuilding(building)
	{
	}

} // core
