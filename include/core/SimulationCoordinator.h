#pragma once


namespace core
{
	class Building;

	// Runs the simulation on behalf of the Building that owns it.
	//
	// Building keeps the world structure and owns every entity registry
	// (ADR 0001), and remains the facade (design pattern, not the Facade sector
	// type of ADR 0003) through which every caller - Agent included - reaches
	// simulation behaviour. The coordinator is where that behaviour lives:
	// traversal request and permit lifecycle, queue tickets and queue positions,
	// door/lift/shuttle/platform-lift/extensible allocation, interactions and
	// device operations, agent lifecycle, and the tick pipeline.
	//
	// ADR 0004 moves that behaviour in staged, leaf-first increments so each
	// stage builds green. This scaffold holds none of it yet: Building
	// constructs one coordinator per Building, and the coordinator is a friend
	// of Agent and of the coordination types in Coordination.h alongside their
	// existing Building friendship, which makes every later stage a pure code
	// move.
	//
	// The name deliberately avoids "controller", which CONTEXT.md bans, and
	// echoes ADR 0001's traversal-coordination vocabulary while covering the
	// wider simulation scope.
	class SimulationCoordinator
	{
	public:

		explicit SimulationCoordinator(Building& building);

		SimulationCoordinator(SimulationCoordinator const&) = delete;
		SimulationCoordinator& operator=(SimulationCoordinator const&) = delete;

		virtual ~SimulationCoordinator() = default;

	private:

		// The coordinator owns no state. It reaches the registries it drives
		// through the Building that owns it.
		Building& mBuilding;
	};

} // core
