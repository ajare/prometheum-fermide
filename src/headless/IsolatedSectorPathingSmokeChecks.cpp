#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/Vertex.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// Regression for the door-test-1 editor crash: an Agent standing in a
	// Location the Graph serves no vertices for (a Corridor with no traversable
	// threshold anywhere) used to make calculatePath throw GraphException
	// ("Sector not found in Sector->Vertex lookup"), which escaped the Ctrl+P
	// path-set flow and terminated the editor. An isolated Location offers no
	// route at all; that is an ordinary "no path" outcome and must come back
	// as no Path, the same as any unreachable target.
	void pathingAcrossAnOpenSharedWallFindsAPath()
	{
		// Minimal reproduction of Agent 15 in door-test-1.world.yaml: the Agent starts
		// in the right-hand Corridor, whose left wall is open into the adjacent
		// Room containing the destination Marker.
		core::World world("Open shared wall", 16, 6);
		auto const room = world.addRoom("Destination room", 0, 2, 6, 4, 1);
		auto const corridor = world.addCorridor(0, 2, 10, 6, 1);
		uint32_t markerIdentifier = 0;
		world.addSectorMarker(room, 0, 0.5625f, &markerIdentifier);
		world.pauseSimulation();
		world.removeLocationWall(corridor, 0, CORE_SIDE_LEFT);
		world.finishBuild();
		world.resumeSimulation();

		auto const agentId = world.createAgent("Agent 15", corridor, 0, 1.484375f);
		auto const agent = world.lookupAgent(agentId).entity;
		auto const destination = world.getGraph()->getVertexByIdentifier(markerIdentifier);
		require(agent != nullptr && destination != nullptr,
			"The open-wall pathing fixture was not constructed");

		auto const path = world.getGraph()->calculatePath(agent, destination);
		require(path && path->nodes.size() >= 2,
			"Agent 15 cannot path from the Corridor through its open wall to the Room Marker");
		require(path->nodes.front().targetVertex->getSector()->getIndex() == corridor
			&& path->nodes.back().targetVertex->getSector()->getIndex() == room,
			"The open-wall route does not cross from the Corridor into the Room");

		agent->setPath(path, true);
		for (uint32_t tick = 0; tick < 1000 && agent->getState() != core::Agent::State::Idle; ++tick)
			world.advanceTick();
		require(agent->getState() == core::Agent::State::Idle
			&& agent->getSector()->getIndex() == room
			&& agent->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f,
			"Agent 15 did not traverse the open wall and reach the Room Marker");
	}

	void pathingFromAnIsolatedLocationReturnsNoPath()
	{
		core::World world("Isolated corridor", 12, 1);

		// The stranded Corridor is split from the connected part of the
		// world by a Room, with no Door or traversable Window anywhere, so
		// the Graph builds no vertices for it.
		auto const stranded = world.addCorridor(0, 0, 3);
		world.addRoom("Blocker", 0, 0, 3, 2, 1);
		auto const connected = world.addCorridor(0, 5, 4);
		uint32_t markerIdentifier = 0;
		world.addSectorMarker(connected, 0, 1.5f, &markerIdentifier);
		world.finishBuild();

		auto const destination = world.getGraph()->getVertexByIdentifier(markerIdentifier);
		require(destination != nullptr,
			"The connected Corridor's Marker has no Graph vertex; the scenario is not wired as intended");

		// Control: an Agent in the connected Corridor reaches the Marker, so
		// the World's graph really is traversable.
		auto const settledAgentId = world.createAgent("Settled agent", connected, 0, 2.5f);
		auto const settledAgent = world.lookupAgent(settledAgentId).entity;
		require(settledAgent != nullptr, "The settled agent was not created");
		auto const settledPath = world.getGraph()->calculatePath(settledAgent, destination);
		require(settledPath && !settledPath->nodes.empty(),
			"No path exists inside the connected Corridor; the scenario is not wired as intended");

		// The regression: the route request out of the stranded Corridor must
		// not throw.
		auto const strandedAgentId = world.createAgent("Stranded agent", stranded, 0, 1.0f);
		auto const strandedAgent = world.lookupAgent(strandedAgentId).entity;
		require(strandedAgent != nullptr, "The stranded agent was not created");

		auto const path = world.getGraph()->calculatePath(strandedAgent, destination);
		require(!path || path->nodes.empty(),
			"A route was found out of an isolated Corridor with no traversable threshold");
	}
}

void runIsolatedSectorPathingSmokeChecks()
{
	pathingAcrossAnOpenSharedWallFindsAPath();
	pathingFromAnIsolatedLocationReturnsNoPath();
}
