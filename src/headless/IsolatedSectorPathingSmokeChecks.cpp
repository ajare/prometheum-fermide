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

	void roomRoutesStayOnConnectedFloor()
	{
		core::World world("Multi-level route source", 8, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 8, 2);
		for (uint32_t x = 2; x < 8; ++x) world.addSectorWalkway(room, 1, x);
		uint32_t marker = 0;
		world.addSectorMarker(room, 1, 2.75f, &marker);
		world.addSectorLadder(room, 0, 6, { 2, false, true });
		world.finishBuild();
		auto agent = world.lookupAgent(world.createAgent("Agent 26", room, 0, 2.75f)).entity;
		auto target = world.getGraph()->getVertexByIdentifier(marker);
		auto path = world.getGraph()->calculatePath(agent, target);
		require(path && path->nodes.front().targetVertex->getPosition().y == 0.0f,
			"Room route starts on another level, bypassing the Ladder");
		agent->setPath(path, true);
		for (uint32_t tick = 0; tick < 4000 && agent->getState() != core::Agent::State::Idle; ++tick)
		{
			auto before = agent->getGlobalPosition();
			world.advanceTick();
			auto after = agent->getGlobalPosition();
			require(after.y == before.y || after.x == before.x,
				"Agent moves diagonally between levels instead of climbing the Ladder");
		}
		require(agent->getGlobalPosition().distanceTo(target->getPosition()) < 0.001f,
			"Agent did not reach the destination using the Ladder");
	}

	void queuedClimbersReachTheMountBeforeClimbing()
	{
		core::World world("Queued Room Ladder", 8, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 8, 2);
		for (uint32_t x = 0; x < 8; ++x) world.addSectorWalkway(room, 1, x);
		uint32_t marker = 0;
		world.addSectorMarker(room, 1, 7.5f, &marker);
		world.addSectorLadder(room, 0, 3, { 2, false, true });
		world.finishBuild();
		std::vector<core::Agent*> agents;
		for (uint32_t i = 0; i < 4; ++i)
		{
			auto agent = world.lookupAgent(world.createAgent("Climber", room, 0, 1.0f + i * 0.5f)).entity;
			agents.push_back(agent);
			agent->setPath(world.getGraph()->calculatePath(agent,
				world.getGraph()->getVertexByIdentifier(marker)), true);
		}
		for (uint32_t tick = 0; tick < 4000; ++tick)
		{
			std::vector<core::Vector2> positions;
			for (auto agent : agents) positions.push_back(agent->getGlobalPosition());
			world.advanceTick();
			for (size_t i = 0; i < agents.size(); ++i)
			{
				auto after = agents[i]->getGlobalPosition();
				require(after.y == positions[i].y || after.x == positions[i].x,
					"Queued Agent climbs diagonally from its queue position");
			}
		}
		for (auto agent : agents)
			require(agent->getGlobalPosition().distanceTo({ 7.5f, 1.0f }) < 0.001f,
				"Queued climber did not reach the destination");
	}

	void roomRoutesCannotStartAcrossAWalkwayGap()
	{
		core::World world("Disconnected walkways", 8, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 8, 2);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 2);
		uint32_t marker = 0;
		world.addSectorMarker(room, 1, 2.1f, &marker);
		world.finishBuild();
		auto agent = world.lookupAgent(world.createAgent("Stranded", room, 1, 0.9f)).entity;
		auto path = world.getGraph()->calculatePath(agent, world.getGraph()->getVertexByIdentifier(marker));
		require(!path || path->nodes.empty(), "Agent route crosses a Walkway gap without a connection");
	}
}

void runIsolatedSectorPathingSmokeChecks()
{
	roomRoutesStayOnConnectedFloor();
	roomRoutesCannotStartAcrossAWalkwayGap();
	queuedClimbersReachTheMountBeforeClimbing();
	pathingAcrossAnOpenSharedWallFindsAPath();
	pathingFromAnIsolatedLocationReturnsNoPath();
}
