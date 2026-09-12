#include <bit>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "core/Agent.h"
#include "core/Building.h"
#include "core/Graph.h"
#include "core/Simulation.h"
#include "core/Vector2.h"

static_assert(!std::is_convertible_v<core::AgentId, core::InteractionPointId>);
static_assert(!std::is_convertible_v<core::DeviceOperationId, core::TraversalResourceId>);

namespace
{
	constexpr uint64_t MaximumSimulationTicks = 1000;

	struct ScenarioResult
	{
		bool reachedDestination{ false };
		core::SimulationSnapshot snapshot;
		std::vector<core::SimulationEvent> events;
	};

	void appendAgent(std::ostringstream& output, core::AgentSnapshot const& agent)
	{
		output << agent.id.value << ':' << agent.name << ':' << agent.sectorId.value << ':'
			<< std::bit_cast<uint32_t>(agent.localPosition.x) << ':'
			<< std::bit_cast<uint32_t>(agent.localPosition.y) << ':'
			<< std::bit_cast<uint32_t>(agent.globalPosition.x) << ':'
			<< std::bit_cast<uint32_t>(agent.globalPosition.y) << ':'
			<< (int)agent.state << ':' << agent.hasPath << ':'
			<< agent.targetPathNode << ':' << agent.pathNodeCount;
	}

	std::string canonicalResult(ScenarioResult const& result)
	{
		std::ostringstream output;
		output << result.snapshot.tick << '|';
		for (auto const& agent : result.snapshot.agents)
		{
			appendAgent(output, agent);
			output << '|';
		}

		for (auto const& event : result.events)
		{
			output << event.sequence << ':' << event.tick << ':' << (int)event.type << ':'
				<< (int)event.phase << ':' << event.hasPreviousAgent << ':';
			if (event.hasPreviousAgent)
			{
				appendAgent(output, event.previousAgent);
			}
			output << ':';
			if (event.type != core::SimulationEventType::PhaseCompleted)
			{
				appendAgent(output, event.agent);
			}
			output << '|';
		}
		return output.str();
	}

	bool phasesAreOrdered(std::vector<core::SimulationEvent> const& events, uint64_t ticks)
	{
		constexpr core::SimulationPhase expected[] = {
			core::SimulationPhase::ResourceAdvancement,
			core::SimulationPhase::IntentCollection,
			core::SimulationPhase::Allocation,
			core::SimulationPhase::Movement,
			core::SimulationPhase::Commit,
			core::SimulationPhase::CleanupAndEventPublication
		};

		uint64_t phaseEventCount = 0;
		for (auto const& event : events)
		{
			if (event.type != core::SimulationEventType::PhaseCompleted)
			{
				continue;
			}
			if (event.phase != expected[phaseEventCount % std::size(expected)])
			{
				return false;
			}
			++phaseEventCount;
		}
		return phaseEventCount == ticks * std::size(expected);
	}

	bool accumulatedRenderTimeAdvancesWholeTicksOnly()
	{
		core::Building building("Accumulator check", 1, 1);
		auto halfTick = core::Building::getFixedTimestep() * 0.5f;
		building.update(halfTick);
		if (building.getSimulationTick() != 0)
		{
			return false;
		}
		building.update(halfTick);
		return building.getSimulationTick() == 1;
	}

	bool buildingOwnsTypedEntitiesAndInvalidatesHandles()
	{
		core::Building building("Ownership check", 3, 2);
		auto corridor = building.addCorridor(0, 0, 2);
		building.finishBuild();

		auto agentId = building.createAgent("Owned idle agent", corridor, 0, 0.5f);
		auto pointId = building.createInteractionPoint("Light switch");
		auto operationId = building.createDeviceOperation("Turn lights on", agentId);
		auto resourceId = building.createTraversalResource("Ordinary passage");

		auto snapshot = building.getSimulationSnapshot();
		if (snapshot.agents.size() != 1 || snapshot.agents.front().id != agentId
			|| snapshot.interactionPoints.size() != 1 || snapshot.interactionPoints.front().id != pointId
			|| snapshot.deviceOperations.size() != 1 || snapshot.deviceOperations.front().id != operationId
			|| snapshot.deviceOperations.front().requester != agentId
			|| snapshot.traversalResources.size() != 1 || snapshot.traversalResources.front().id != resourceId)
		{
			return false;
		}

		if (!building.removeAgent(agentId)
			|| building.lookupAgent(agentId)
			|| building.lookupAgent(agentId).diagnostic.empty()
			|| building.lookupDeviceOperation(operationId)
			|| building.lookupDeviceOperation(operationId).diagnostic.empty())
		{
			return false;
		}
		if (!building.removeInteractionPoint(pointId) || !building.removeTraversalResource(resourceId))
		{
			return false;
		}

		building.advanceTick();
		auto afterRemoval = building.getSimulationSnapshot();
		return afterRemoval.agents.empty()
			&& afterRemoval.interactionPoints.empty()
			&& afterRemoval.deviceOperations.empty()
			&& afterRemoval.traversalResources.empty();
	}

	ScenarioResult runOrdinaryPathScenario()
	{
		core::Building building("Headless smoke building", 7, 2);
		auto corridor = building.addCorridor(0, 0, 6);

		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		building.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		building.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		building.finishBuild();

		auto agentId = building.createAgent("Headless smoke agent", corridor, 0, 0.5f);
		auto agentLookup = building.lookupAgent(agentId);
		if (!agentLookup)
		{
			return {};
		}
		auto agent = agentLookup.entity;

		auto graph = building.getGraph();
		auto source = graph->getVertexByIdentifier(sourceVertexId);
		auto destination = graph->getVertexByIdentifier(destinationVertexId);
		auto path = graph->calculatePath(agent, source, destination);
		if (!path)
		{
			return {};
		}
		agent->setPath(path, true);

		while (agent->getState() != core::Agent::State::Idle
			&& building.getSimulationTick() < MaximumSimulationTicks)
		{
			building.advanceTick();
		}

		ScenarioResult result;
		result.snapshot = building.getSimulationSnapshot();
		result.events = building.consumeSimulationEvents();
		auto finalPosition = agent->getGlobalPosition();
		result.reachedDestination = agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == building.getSector(corridor).get()
			&& finalPosition.distanceTo(destination->getPosition()) < 0.001f
			&& phasesAreOrdered(result.events, result.snapshot.tick);
		return result;
	}
}

int main()
{
	try
	{
		if (!accumulatedRenderTimeAdvancesWholeTicksOnly())
		{
			std::cerr << "FAIL: render-time accumulation did not advance exactly one whole tick\n";
			return 1;
		}
		if (!buildingOwnsTypedEntitiesAndInvalidatesHandles())
		{
			std::cerr << "FAIL: typed building ownership or handle invalidation failed\n";
			return 1;
		}

		auto first = runOrdinaryPathScenario();
		auto second = runOrdinaryPathScenario();
		if (!first.reachedDestination || !second.reachedDestination)
		{
			std::cerr << "FAIL: deterministic ordinary path scenario did not complete\n";
			return 1;
		}
		if (canonicalResult(first) != canonicalResult(second))
		{
			std::cerr << "FAIL: repeated runs produced different snapshots or events\n";
			return 1;
		}

		auto const& agent = first.snapshot.agents.front();
		std::cout << "PASS: deterministic snapshot and events matched after "
			<< first.snapshot.tick << " fixed ticks; final position=("
			<< agent.globalPosition.x << ", " << agent.globalPosition.y << ")\n";
		return 0;
	}
	catch (std::exception const& exception)
	{
		std::cerr << "FAIL: headless smoke scenario threw: " << exception.what() << '\n';
		return 1;
	}
	catch (...)
	{
		std::cerr << "FAIL: headless smoke scenario threw an unknown exception\n";
		return 1;
	}
}
