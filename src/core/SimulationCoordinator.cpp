#include "core/SimulationCoordinator.h"

#include "core/Building.h"


namespace core
{

	// No simulation behaviour has moved here yet (ADR 0004). Later stages move it
	// in leaf-first increments, splitting the implementation per seam across
	// further SimulationCoordinator*.cpp translation units behind this single
	// header so no translation unit recreates the size problem the refactor
	// removes.
	SimulationCoordinator::SimulationCoordinator(Building& building)
		: mBuilding(building)
	{
	}

} // core
