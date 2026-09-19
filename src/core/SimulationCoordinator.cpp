#include "core/SimulationCoordinator.h"

#include "core/Building.h"


namespace core
{

	// ADR 0004 moves simulation behaviour in leaf-first increments, splitting
	// the implementation per seam across further SimulationCoordinator*.cpp
	// translation units behind this single header so no translation unit
	// recreates the size problem the refactor removes. Agent lifecycle lives in
	// SimulationCoordinatorAgents.cpp and interactions and device operations in
	// SimulationCoordinatorInteractions.cpp; the remaining seam families -
	// allocation, queues and admission, the tick pipeline - follow in later
	// stages.
	SimulationCoordinator::SimulationCoordinator(Building& building)
		: mBuilding(building)
	{
	}

} // core
