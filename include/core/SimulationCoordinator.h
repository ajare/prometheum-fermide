#pragma once

#include <memory>
#include <string>

#include "core/Coordination.h"
#include "core/EntityId.h"


namespace core
{
	class Building;
	class Agent;

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
	// stage builds green. The coordinator owns no state: it works on Building's
	// registries through the Building it was given, and calls back through the
	// Building facade for machinery which has not moved out of Building yet.
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

		// ------------------------------------------------------------------
		// Agent lifecycle (ADR 0004 stage 1)
		//
		// Creating an Agent, placing it in a Sector, removing it, waking every
		// Agent, resolving Agents between handles and pointers, and releasing an
		// Agent's traversal ownership all live here. Building forwards each of
		// these entry points; no caller outside Building names the coordinator.
		// ------------------------------------------------------------------

		// Creates an Agent named `name` and places it in the Sector `sectorId`,
		// either at a specific deck and lateral offset or at the Sector default.
		AgentId createAgent(std::string const& name, uint32_t sectorId, uint32_t deckOffset, float xOffset);

		AgentId createAgent(std::string const& name, uint32_t sectorId);

		// Takes an already-constructed Agent into the Building's ownership and
		// places it in the Sector `sectorId`. The Agent is attached to the
		// Building, never to the coordinator: the Building remains the owner the
		// Agent reports to.
		AgentId addOwnedAgentToSector(std::unique_ptr<Agent> agent, uint32_t sectorId, uint32_t deckOffset, float xOffset);

		AgentId addOwnedAgentToSector(std::unique_ptr<Agent> agent, uint32_t sectorId);

		// Handle resolution. A lookup reports the Agent behind a handle plus a
		// diagnostic when the handle is dead; `getAgentId` is its inverse and
		// yields an empty handle for an Agent this Building does not own.
		EntityLookup<Agent> lookupAgent(AgentId id);

		EntityLookup<Agent const> lookupAgent(AgentId id) const;

		AgentId getAgentId(Agent const* agent) const;

		// Removes the Agent behind `id`, releasing it from every traversal
		// resource, interaction request, and device operation that names it
		// before the entity itself goes. A refusal leaves the Agent untouched.
		EntityRemovalResult removeAgent(AgentId id);

		// Wakes every Agent the Building owns.
		void wakeAllAgents();

		// Traversal-ownership release. An Agent's claims on a traversal
		// resource - a manifest slot, a stop request, an occupant lease -
		// outlive nothing, so they are surrendered before the Agent is destroyed
		// (ticket #57).
		bool holdsTraversalOwnership(AgentId id) const;

		void releaseAgentFromResource(TraversalResource& resource, AgentId id);

		void releaseTraversalOwnership(AgentId id);

	private:

		// The coordinator owns no state. It reaches the registries it drives
		// through the Building that owns it.
		Building& mBuilding;
	};

} // core
