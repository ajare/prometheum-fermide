#include <algorithm>
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
#include "core/GapEdge.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/SectorEdge.h"
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

	void appendTraversalRequest(std::ostringstream& output, core::TraversalRequestSnapshot const& request)
	{
		output << request.id.value << ':' << request.owner.value << ':' << (int)request.edgeType << ':'
			<< request.sourceSector.value << ':' << request.destinationSector.value << ':'
			<< std::bit_cast<uint32_t>(request.sourceEndpoint.x) << ':'
			<< std::bit_cast<uint32_t>(request.sourceEndpoint.y) << ':'
			<< std::bit_cast<uint32_t>(request.destinationEndpoint.x) << ':'
			<< std::bit_cast<uint32_t>(request.destinationEndpoint.y) << ':'
			<< (int)request.state << ':' << request.permit.value;
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
			switch (event.type)
			{
			case core::SimulationEventType::AgentAdded:
			case core::SimulationEventType::AgentChanged:
			case core::SimulationEventType::AgentRemoved:
				appendAgent(output, event.agent);
				break;
			case core::SimulationEventType::TraversalRequestAdded:
			case core::SimulationEventType::TraversalRequestChanged:
			case core::SimulationEventType::TraversalRequestRemoved:
				appendTraversalRequest(output, event.traversalRequest);
				break;
			case core::SimulationEventType::TraversalPermitAdded:
			case core::SimulationEventType::TraversalPermitChanged:
			case core::SimulationEventType::TraversalPermitRemoved:
				output << event.traversalPermit.id.value << ':' << event.traversalPermit.request.value
					<< ':' << event.traversalPermit.owner.value << ':' << (int)event.traversalPermit.state;
				break;
			default:
				break;
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

	std::shared_ptr<core::Path> twoNodePath(std::shared_ptr<const core::Vertex> source,
		std::shared_ptr<const core::Vertex> destination, std::shared_ptr<const core::Edge> edge)
	{
		auto path = std::make_shared<core::Path>();
		path->nodes.push_back({ nullptr, std::move(source), 0.0f });
		path->nodes.push_back({ std::move(edge), std::move(destination), 1.0f });
		return path;
	}

	bool ordinaryTraversalCommitsOnlyAtDestination()
	{
		core::Building building("Ordinary transition", 10, 2);
		auto sourceSector = building.addCorridor(0, 0, 3);
		auto destinationSector = building.addCorridor(0, 5, 3);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		building.addSectorMarker(sourceSector, 0, 0.5f, &sourceVertexId);
		building.addSectorMarker(destinationSector, 0, 1.5f, &destinationVertexId);
		building.finishBuild();

		auto source = building.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = building.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = building.createAgent("Ordinary traveller", sourceSector, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::SectorEdge>()), true);

		bool observedPermit = false;
		while (agent->getState() != core::Agent::State::Idle
			&& building.getSimulationTick() < MaximumSimulationTicks)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			if (!snapshot.traversalPermits.empty())
			{
				observedPermit = snapshot.traversalPermits.size() == 1
					&& snapshot.traversalRequests.size() == 1
					&& snapshot.agents.front().hasLocomotionTask;
			}

			if (agent->getState() != core::Agent::State::Idle
				&& agent->getSector() != building.getSector(sourceSector).get())
			{
				return false;
			}
		}

		auto snapshot = building.getSimulationSnapshot();
		return observedPermit
			&& agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == building.getSector(destinationSector).get()
			&& agent->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& snapshot.traversalRequests.empty()
			&& snapshot.traversalPermits.empty();
	}

	bool deniedTraversalCannotBeCrossed()
	{
		core::Building building("Denied transition", 7, 2);
		auto corridor = building.addCorridor(0, 0, 6);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		building.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		building.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		building.finishBuild();

		auto source = building.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = building.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = building.createAgent("Blocked traveller", corridor, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::GapEdge>()), true);
		building.advanceTicks(30);

		auto snapshot = building.getSimulationSnapshot();
		if (agent->getGlobalPosition().distanceTo(source->getPosition()) >= 0.001f
			|| agent->getSector() != building.getSector(corridor).get()
			|| agent->getState() != core::Agent::State::WaitingForTraversal
			|| snapshot.traversalRequests.size() != 1
			|| snapshot.traversalRequests.front().state != core::TraversalRequestState::Denied
			|| !snapshot.traversalPermits.empty())
		{
			return false;
		}

		agent->clearPath();
		snapshot = building.getSimulationSnapshot();
		return snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty()
			&& agent->getSector() == building.getSector(corridor).get();
	}

	bool cancellationReleasesPermitWithoutCommitting()
	{
		core::Building building("Cancelled transition", 10, 2);
		auto sourceSector = building.addCorridor(0, 0, 3);
		auto destinationSector = building.addCorridor(0, 5, 3);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		building.addSectorMarker(sourceSector, 0, 0.5f, &sourceVertexId);
		building.addSectorMarker(destinationSector, 0, 1.5f, &destinationVertexId);
		building.finishBuild();

		auto source = building.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = building.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = building.createAgent("Cancelling traveller", sourceSector, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::SectorEdge>()), true);

		for (uint32_t i = 0; i < 10 && !agent->getTraversalPermitId(); ++i)
		{
			building.advanceTick();
		}
		if (!agent->getTraversalPermitId() || agent->getSector() != building.getSector(sourceSector).get())
		{
			return false;
		}

		agent->clearPath();
		building.advanceTicks(10);
		auto snapshot = building.getSimulationSnapshot();
		return agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == building.getSector(sourceSector).get()
			&& snapshot.traversalRequests.empty()
			&& snapshot.traversalPermits.empty();
	}

	bool typedLightingInteractionCoalescesAndCancelsByRequester()
	{
		core::Building building("Typed lighting interaction", 6, 2);
		auto corridorIndex = building.addCorridor(0, 0, 5);
		building.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto firstAgent = building.createAgent("First operator", corridorIndex, 0, 0.5f);
		auto secondAgent = building.createAgent("Dependent operator", corridorIndex, 0, 0.7f);

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		binding.requirement = core::InteractionBindingRequirement::Required;
		auto point = building.createInteractionPoint("Typed light control", sectorId,
			{ 3.5f, 0.5f }, 0.1f, core::Building::getFixedTimestep() * 3.0f, { binding });
		auto firstRequest = building.requestInteraction(point, firstAgent);
		auto secondRequest = building.requestInteraction(point, secondAgent);
		if (!firstRequest || !secondRequest)
		{
			return false;
		}

		auto first = building.lookupInteractionRequest(firstRequest);
		auto second = building.lookupInteractionRequest(secondRequest);
		if (!first || !second || first.entity->getOperations().size() != 1
			|| second.entity->getOperations().size() != 1
			|| first.entity->getOperations().front().first != second.entity->getOperations().front().first)
		{
			return false;
		}
		auto operationId = first.entity->getOperations().front().first;

		for (uint32_t i = 0; i < MaximumSimulationTicks; ++i)
		{
			building.advanceTick();
			auto operation = building.lookupDeviceOperation(operationId);
			if (operation && operation.entity->getState() == core::DeviceOperationState::Running)
			{
				break;
			}
		}
		auto firstPosition = building.lookupAgent(firstAgent).entity->getGlobalPosition();
		if (firstPosition.distanceTo({ 3.5f, 0.5f }) > 0.101f || !building.cancelInteraction(firstRequest))
		{
			return false;
		}
		auto operation = building.lookupDeviceOperation(operationId);
		if (!operation || operation.entity->getState() == core::DeviceOperationState::Cancelled
			|| operation.entity->getRequesters().size() != 1
			|| !operation.entity->getRequesters().contains(secondAgent))
		{
			return false;
		}

		building.advanceTicks(3);
		first = building.lookupInteractionRequest(firstRequest);
		second = building.lookupInteractionRequest(secondRequest);
		auto snapshot = building.getSimulationSnapshot();
		return first && first.entity->getResult() == core::InteractionResult::Cancelled
			&& second && second.entity->getResult() == core::InteractionResult::Succeeded
			&& !building.getSector(corridorIndex)->areLightsOn()
			&& snapshot.deviceOperations.size() == 1
			&& snapshot.deviceOperations.front().hasCommand
			&& snapshot.deviceOperations.front().command.type == core::DeviceCommandType::SetSectorLights
			&& snapshot.deviceOperations.front().state == core::DeviceOperationState::Succeeded
			&& snapshot.interactionPoints.front().activeRequest == core::InteractionRequestId{};
	}

	bool singleAgentDoorJourney(core::DoorActivationMode mode)
	{
		core::Building building("Single-agent door", 6, 2);
		auto fore = building.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 5, 1);
		auto back = building.addRoom("Back", CORE_LAYER_BACK, 0, 0, 5, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = mode;
		options.holdOpenSeconds = core::Building::getFixedTimestep() * 8.0f;
		auto created = building.addSectorDoor(0, 2, options);
		building.finishBuild();

		auto edgeIt = std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& edge) { return edge->getType() == core::EdgeType::Door; });
		if (edgeIt == building.getGraph()->getEdges().end() || !created.traversalResource)
		{
			return false;
		}
		auto edge = *edgeIt;
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = building.createAgent("Door traveller", fore, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);

		bool observedWaitingForFullOpen = false;
		bool observedVisibleCrossingLease = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks && agent->getState() != core::Agent::State::Idle; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			auto const& resource = snapshot.traversalResources.front();
			if (resource.doorState == core::DoorSnapshotState::Opening
				&& snapshot.traversalPermits.empty()
				&& agent->getSector() == building.getSector(fore).get())
			{
				observedWaitingForFullOpen = true;
			}
			if (agent->getState() == core::Agent::State::TraversingEdge
				&& resource.doorState == core::DoorSnapshotState::Open
				&& resource.openLeaseCount == 1
				&& agent->getSector() == building.getSector(fore).get())
			{
				observedVisibleCrossingLease = true;
			}
		}

		auto completed = building.getSimulationSnapshot();
		if (!observedWaitingForFullOpen || !observedVisibleCrossingLease
			|| agent->getState() != core::Agent::State::Idle
			|| agent->getSector() != building.getSector(back).get()
			|| completed.deviceOperations.size() != 1
			|| completed.deviceOperations.front().command.type != core::DeviceCommandType::OpenDoor
			|| completed.deviceOperations.front().state != core::DeviceOperationState::Succeeded
			|| completed.traversalResources.front().doorActivationMode != mode
			|| completed.traversalResources.front().openLeaseCount != 0)
		{
			return false;
		}

		for (uint32_t i = 0; i < 120
			&& building.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Closed; ++i)
		{
			building.advanceTick();
		}
		return building.getSimulationSnapshot().traversalResources.front().doorState == core::DoorSnapshotState::Closed;
	}

	bool remoteDoorUsesOnePhysicalOperatorAndSharedOperation()
	{
		core::Building building("Shared remote door", 7, 2);
		auto fore = building.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 6, 1);
		auto back = building.addRoom("Back", CORE_LAYER_BACK, 0, 0, 6, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controllers[0] = true;
		options.controllers[1] = true;
		options.orchestrate = true;
		auto created = building.addSectorDoor(0, 3, options);
		building.finishBuild();

		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto firstId = building.createAgent("First remote waiter", fore, 0, 0.4f);
		auto secondId = building.createAgent("Second remote waiter", fore, 0, 0.6f);
		auto first = building.lookupAgent(firstId).entity;
		auto second = building.lookupAgent(secondId).entity;
		first->setPath(twoNodePath(source, destination, edge), true);
		second->setPath(twoNodePath(source, destination, edge), true);

		bool observedSharedPreparation = false;
		bool cancelledFirst = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& second->getState() != core::Agent::State::Idle; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			if (!cancelledFirst && snapshot.traversalRequests.size() == 2
				&& snapshot.interactionRequests.size() == 1
				&& snapshot.deviceOperations.size() == 1
				&& snapshot.deviceOperations.front().requesters.size() == 2
				&& snapshot.traversalResources.front().preparationOperator
				&& snapshot.traversalResources.front().activePreparation)
			{
				observedSharedPreparation = true;
				first->clearPath();
				cancelledFirst = true;
			}
		}

		auto snapshot = building.getSimulationSnapshot();
		return observedSharedPreparation && cancelledFirst
			&& first->getSector() == building.getSector(fore).get()
			&& second->getState() == core::Agent::State::Idle
			&& second->getSector() == building.getSector(back).get()
			&& snapshot.deviceOperations.size() >= 1
			&& std::any_of(snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(), [](auto const& operation)
				{ return operation.command.type == core::DeviceCommandType::OpenDoor
					&& operation.state == core::DeviceOperationState::Succeeded; });
	}

	bool remoteDoorWithoutReachableControlIsUnavailable()
	{
		core::Building building("Uncontrolled remote door", 6, 2);
		auto fore = building.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 5, 1);
		building.addRoom("Back", CORE_LAYER_BACK, 0, 0, 5, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controllers[0] = false;
		options.controllers[1] = false;
		auto created = building.addSectorDoor(0, 2, options);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = building.createAgent("Stranded remote waiter", fore, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);
		building.advanceTicks(400);
		auto snapshot = building.getSimulationSnapshot();
		return snapshot.traversalRequests.size() == 1
			&& snapshot.traversalRequests.front().state == core::TraversalRequestState::Denied
			&& snapshot.traversalRequests.front().failureReason == core::TraversalFailureReason::NoReachableControl
			&& snapshot.deviceOperations.empty() && snapshot.interactionRequests.empty();
	}

	bool fairDoorQueuesServeBothSidesInStableOrder()
	{
		core::Building building("Fair two-sided door", 8, 2);
		auto fore = building.addRoom("Fore queue", CORE_LAYER_FORE, 0, 0, 7, 1);
		auto back = building.addRoom("Back queue", CORE_LAYER_BACK, 0, 0, 7, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = building.addSectorDoor(0, 3, options);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto foreVertex = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto backVertex = edge->getOtherVertex(foreVertex);

		std::vector<core::AgentId> ids = {
			building.createAgent("Fore first", fore, 0, 3.5f),
			building.createAgent("Back first", back, 0, 3.5f),
			building.createAgent("Fore second", fore, 0, 3.5f),
			building.createAgent("Back second", back, 0, 3.5f)
		};
		for (size_t i = 0; i < ids.size(); ++i)
		{
			auto source = i % 2 == 0 ? foreVertex : backVertex;
			auto destination = i % 2 == 0 ? backVertex : foreVertex;
			building.lookupAgent(ids[i]).entity->setPath(twoNodePath(source, destination, edge), true);
		}

		bool observedSeparatedPositions = false;
		bool observedQueueDiagnostics = false;
		std::vector<core::AgentId> completionOrder;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2 && completionOrder.size() < ids.size(); ++tick)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			if (snapshot.traversalPermits.size() > 1)
			{
				return false;
			}
			auto const& resource = snapshot.traversalResources.front();
			observedQueueDiagnostics = observedQueueDiagnostics
				|| (resource.queueLanes.size() == 2 && resource.crossingOwner);
			for (auto const& lane : resource.queueLanes)
			{
				std::vector<core::Vector2> occupied;
				for (auto const& position : lane.positions)
				{
					if (!position.owner)
					{
						continue;
					}
					for (auto const& other : occupied)
					{
						if (position.position.distanceTo(other) < CORE_DOOR_QUEUE_STOP_WIDTH - 0.001f)
						{
							return false;
						}
					}
					occupied.push_back(position.position);
				}
				observedSeparatedPositions = observedSeparatedPositions || occupied.size() >= 2;
			}
			for (auto id : ids)
			{
				if (std::find(completionOrder.begin(), completionOrder.end(), id) == completionOrder.end()
					&& building.lookupAgent(id).entity->getState() == core::Agent::State::Idle)
				{
					completionOrder.push_back(id);
				}
			}
		}
		return completionOrder == ids && observedSeparatedPositions && observedQueueDiagnostics
			&& building.getSimulationSnapshot().traversalResources.front().crossingOwner == core::TraversalRequestId{};
	}

	bool queuedCancellationReleasesAndAdvancesPositions()
	{
		core::Building building("Queue cancellation", 8, 2);
		auto fore = building.addRoom("Queue room", CORE_LAYER_FORE, 0, 0, 7, 1);
		building.addRoom("Destination", CORE_LAYER_BACK, 0, 0, 7, 1);
		auto created = building.addSectorDoor(0, 3);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto firstId = building.createAgent("First", fore, 0, 3.5f);
		auto cancelledId = building.createAgent("Cancelled", fore, 0, 3.5f);
		auto lastId = building.createAgent("Last", fore, 0, 3.5f);
		for (auto id : { firstId, cancelledId, lastId })
		{
			building.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		}
		building.advanceTicks(3);
		auto before = building.getSimulationSnapshot();
		if (before.traversalRequests.size() != 3
			|| std::count_if(before.traversalRequests.begin(), before.traversalRequests.end(),
				[](auto const& request) { return request.queueTicket && request.hasQueuePosition; }) != 3)
		{
			return false;
		}
		building.lookupAgent(cancelledId).entity->clearPath();
		auto after = building.getSimulationSnapshot();
		if (after.traversalRequests.size() != 2
			|| after.traversalResources.front().queueLanes.front().queue.size() != 2)
		{
			return false;
		}
		for (auto const& position : after.traversalResources.front().queueLanes.front().positions)
		{
			if (position.owner && position.owner == before.traversalRequests[1].id)
			{
				return false;
			}
		}
		building.advanceTicks(MaximumSimulationTicks);
		return building.lookupAgent(firstId).entity->getState() == core::Agent::State::Idle
			&& building.lookupAgent(lastId).entity->getState() == core::Agent::State::Idle
			&& building.lookupAgent(cancelledId).entity->getSector() == building.getSector(fore).get();
	}

	bool resilientWaitingRetainsPriorityAndExpiresPermits()
	{
		core::Building building("Resilient door waiting", 8, 2);
		auto fore = building.addRoom("Waiting side", CORE_LAYER_FORE, 0, 0, 7, 1);
		building.addRoom("Destination side", CORE_LAYER_BACK, 0, 0, 7, 1);
		auto created = building.addSectorDoor(0, 3);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);

		// One physical position deliberately forces logical overflow.
		auto sourceSectorId = core::SectorId{ (uint64_t)fore + 1 };
		if (!building.configureDoorQueueLane(created.traversalResource, sourceSectorId,
			source->getPosition(), { -1.0f, 0.0f }, 0.0f)) return false;
		std::vector<core::AgentId> ids = {
			building.createAgent("Queue head", fore, 0, 3.5f),
			building.createAgent("Overflow one", fore, 0, 3.5f),
			building.createAgent("Overflow two", fore, 0, 3.5f)
		};
		for (auto id : ids) building.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		building.advanceTicks(2);
		auto queued = building.getSimulationSnapshot();
		if (queued.traversalRequests.size() != 3
			|| std::count_if(queued.traversalRequests.begin(), queued.traversalRequests.end(),
				[](auto const& request) { return (bool)request.queueTicket; }) != 3
			|| std::count_if(queued.traversalRequests.begin(), queued.traversalRequests.end(),
				[](auto const& request) { return request.hasQueuePosition; }) != 1)
		{
			return false;
		}

		auto firstRequest = queued.traversalRequests.front();
		building.lookupAgent(ids.front()).entity->setPath(twoNodePath(source, destination, edge), true);
		auto compatible = building.getSimulationSnapshot();
		auto retained = std::find_if(compatible.traversalRequests.begin(), compatible.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids.front(); });
		if (retained == compatible.traversalRequests.end() || retained->id != firstRequest.id
			|| retained->queueTicket != firstRequest.queueTicket) return false;

		// Changing the immediate authority is incompatible and must release the old
		// logical ticket before a later compatible route can queue afresh.
		auto oldOverflow = *std::find_if(compatible.traversalRequests.begin(), compatible.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[1]; });
		building.lookupAgent(ids[1]).entity->setPath(twoNodePath(source, destination,
			std::make_shared<core::SectorEdge>()), true);
		auto incompatible = building.getSimulationSnapshot();
		if (std::any_of(incompatible.traversalRequests.begin(), incompatible.traversalRequests.end(),
			[&](auto const& request) { return request.id == oldOverflow.id; })) return false;
		building.lookupAgent(ids[1]).entity->setPath(twoNodePath(source, destination, edge), true);
		building.advanceTicks(2);
		auto fresh = building.getSimulationSnapshot();
		auto freshOverflow = std::find_if(fresh.traversalRequests.begin(), fresh.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[1]; });
		if (freshOverflow == fresh.traversalRequests.end() || freshOverflow->queueTicket == oldOverflow.queueTicket)
			return false;

		// Route estimation observes queue demand but creates no coordination state.
		auto requestCount = fresh.traversalRequests.size();
		auto permitCount = fresh.traversalPermits.size();
		if (edge->getWeight(destination, building.lookupAgent(ids.front()).entity, true) <= 0.0f
			|| building.getSimulationSnapshot().traversalRequests.size() != requestCount
			|| building.getSimulationSnapshot().traversalPermits.size() != permitCount) return false;

		// A deliberately short no-progress deadline expires the coincident threshold
		// crossing. The request and ticket survive and a fresh permit is assigned.
		auto policy = building.getTraversalWaitingPolicy();
		policy.permitProgressTimeoutTicks = 1;
		building.setTraversalWaitingPolicy(policy);
		core::TraversalPermitId firstPermit;
		core::TraversalPermitId replacementPermit;
		for (uint32_t i = 0; i < MaximumSimulationTicks && !replacementPermit; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			for (auto const& permit : snapshot.traversalPermits)
			{
				if (permit.owner != ids.front()) continue;
				if (!firstPermit) firstPermit = permit.id;
				else if (permit.id != firstPermit) replacementPermit = permit.id;
			}
		}
		if (!firstPermit || !replacementPermit) return false;
		auto afterExpiry = building.getSimulationSnapshot();
		retained = std::find_if(afterExpiry.traversalRequests.begin(), afterExpiry.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids.front(); });
		if (retained == afterExpiry.traversalRequests.end() || retained->id != firstRequest.id
			|| retained->queueTicket != firstRequest.queueTicket) return false;

		policy.permitProgressTimeoutTicks = 120;
		building.setTraversalWaitingPolicy(policy);
		building.advanceTicks(MaximumSimulationTicks * 2);
		return std::all_of(ids.begin(), ids.end(), [&](auto id)
		{
			auto agent = building.lookupAgent(id).entity;
			return agent->getState() == core::Agent::State::Idle
				&& agent->getSector() == destination->getSector().get();
		}) && building.getSimulationSnapshot().traversalRequests.empty()
			&& building.getSimulationSnapshot().traversalPermits.empty();
	}

	bool wideDoorLanesAndGracefulDisableAreSafe()
	{
		core::Building building("Wide safe door", 9, 2);
		auto fore = building.addRoom("Wide fore", CORE_LAYER_FORE, 0, 0, 8, 1);
		auto back = building.addRoom("Wide back", CORE_LAYER_BACK, 0, 0, 8, 1);
		core::Building::CreateDoorOptions options;
		options.width = 2;
		options.crossingLanes = 2;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = building.addSectorDoor(0, 3, options);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			building.createAgent("Wide first", fore, 0, 4.0f),
			building.createAgent("Wide second", fore, 0, 4.0f),
			building.createAgent("Disabled waiter", fore, 0, 4.0f)
		};
		for (auto id : ids) building.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);

		bool disabledWithTwoCrossings = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			if (snapshot.traversalPermits.size() > 2 || snapshot.traversalResources.front().crossingLanes.size() != 2)
				return false;
			if (!disabledWithTwoCrossings && snapshot.traversalPermits.size() == 2)
			{
				auto const& lanes = snapshot.traversalResources.front().crossingLanes;
				if (!lanes[0].owner || !lanes[1].owner || lanes[0].owner == lanes[1].owner
					|| snapshot.traversalResources.front().crossingLeaseCount != 2)
					return false;
				disabledWithTwoCrossings = building.setTraversalResourceEnabled(created.traversalResource, false);
			}
			if (disabledWithTwoCrossings
				&& building.lookupAgent(ids[0]).entity->getState() == core::Agent::State::Idle
				&& building.lookupAgent(ids[1]).entity->getState() == core::Agent::State::Idle)
				break;
		}
		building.advanceTicks(3);
		auto snapshot = building.getSimulationSnapshot();
		auto denied = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[2]; });
		return disabledWithTwoCrossings
			&& building.lookupAgent(ids[0]).entity->getSector() == building.getSector(back).get()
			&& building.lookupAgent(ids[1]).entity->getSector() == building.getSector(back).get()
			&& building.lookupAgent(ids[2]).entity->getSector() == building.getSector(fore).get()
			&& denied != snapshot.traversalRequests.end()
			&& denied->state == core::TraversalRequestState::Denied
			&& denied->failureReason == core::TraversalFailureReason::ResourceDisabled
			&& snapshot.traversalResources.front().crossingLeaseCount == 0;
	}

	bool doorLeasesAndSensorObservationsPreventUnsafeClosure()
	{
		core::Building building("Door observation safety", 7, 2);
		auto fore = building.addRoom("Sensor fore", CORE_LAYER_FORE, 0, 0, 6, 1);
		building.addRoom("Sensor back", CORE_LAYER_BACK, 0, 0, 6, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		options.holdOpenSeconds = core::Building::getFixedTimestep() * 2.0f;
		auto created = building.addSectorDoor(0, 3, options);
		building.finishBuild();
		auto sensor = core::DoorSensorId{ 1 };
		if (!building.setDoorSensorObservation(created.traversalResource, sensor,
			core::DoorSensorObservation::Presence)) return false;
		building.advanceTicks(90);
		auto lease = building.acquireDoorOpenLease(created.traversalResource);
		building.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Clear);
		building.advanceTicks(30);
		auto snapshot = building.getSimulationSnapshot();
		if (!lease || snapshot.traversalResources.front().doorState != core::DoorSnapshotState::Open
			|| snapshot.traversalResources.front().externalOpenLeaseCount != 1) return false;

		auto actor = building.createAgent("Close operator", fore, 0, 3.5f);
		core::DeviceCommand close;
		close.type = core::DeviceCommandType::OpenDoor;
		close.desiredState = false;
		close.traversalResource = created.traversalResource;
		auto point = building.createInteractionPoint("Close door", core::SectorId{ (uint64_t)fore + 1 },
			{ 3.5f, 0.0f }, 1.0f, 0.0f, { { close, core::InteractionBindingRequirement::Required } });
		auto closeRequest = building.requestInteraction(point, actor);
		building.advanceTicks(4);
		if (building.lookupInteractionRequest(closeRequest).entity->getResult() != core::InteractionResult::Rejected
			|| !building.releaseDoorOpenLease(created.traversalResource, lease)) return false;

		building.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Obstruction);
		building.advanceTicks(20);
		if (building.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Open) return false;
		building.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Clear);
		for (uint32_t i = 0; i < 10 && building.getSimulationSnapshot().traversalResources.front().doorState
			!= core::DoorSnapshotState::Closing; ++i) building.advanceTick();
		if (building.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Closing) return false;
		building.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Obstruction);
		building.advanceTick();
		snapshot = building.getSimulationSnapshot();
		return snapshot.traversalResources.front().obstructionObserved
			&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Opening;
	}

	bool finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed()
	{
		core::Building building("Finite ladder", 4, 4);
		auto lower = building.addCorridor(0, 0, 3);
		auto upper = building.addCorridor(2, 0, 3);
		core::Building::CreateLadderOptions options{ 3, false, true };
		options.agentSpacing = 10.0f; // Deliberately derive a single capacity slot.
		auto created = building.addLadder(0, 1, options);
		building.finishBuild();
		if (!created.traversalResource) return false;

		auto const& graph = building.getGraph();
		auto target = graph->getClosestVertexInSector(building.getSector(upper).get(), { 1.5f, 2.0f });
		if (!target) return false;
		std::vector<core::AgentId> ids = {
			building.createAgent("First climber", lower, 0, 1.5f),
			building.createAgent("Second climber", lower, 0, 1.5f),
			building.createAgent("Cancelled climber", lower, 0, 1.5f)
		};
		for (auto id : ids)
		{
			auto agent = building.lookupAgent(id).entity;
			auto path = graph->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool observedFull = false;
		bool cancelledWaiter = false;
		uint64_t climbStarted = 0;
		uint64_t climbFinished = 0;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 3; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isLadder
				|| resource->capacity != 1
				|| resource->occupantCount + resource->admissionReservationCount > resource->capacity
				|| resource->capacityPositions.size() != resource->capacity)
				return false;
			if (resource->occupantCount == 1)
			{
				observedFull = true;
				if (!climbStarted && building.lookupAgent(ids[0]).entity->getState()
					== core::Agent::State::TraversingEdge)
					climbStarted = building.getSimulationTick();
				if (!cancelledWaiter)
				{
					building.lookupAgent(ids[2]).entity->clearPath();
					cancelledWaiter = true;
				}
			}
			if (climbStarted && !climbFinished
				&& building.lookupAgent(ids[0]).entity->getSector() == building.getSector(upper).get())
				climbFinished = building.getSimulationTick();
			if (building.lookupAgent(ids[0]).entity->getState() == core::Agent::State::Idle
				&& building.lookupAgent(ids[1]).entity->getState() == core::Agent::State::Idle)
				break;
		}

		auto snapshot = building.getSimulationSnapshot();
		auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == created.traversalResource; });
		// Two vertical units at 0.25 units/second require about 480 fixed ticks;
		// this also detects accidentally using walking speed.
		return observedFull && cancelledWaiter && climbStarted && climbFinished
			&& climbFinished - climbStarted >= 470
			&& building.lookupAgent(ids[0]).entity->getSector() == building.getSector(upper).get()
			&& building.lookupAgent(ids[1]).entity->getSector() == building.getSector(upper).get()
			&& building.lookupAgent(ids[2]).entity->getSector() == building.getSector(lower).get()
			&& resource != snapshot.traversalResources.end()
			&& resource->occupantCount == 0 && resource->admissionReservationCount == 0
			&& resource->admissionQueue.empty();
	}

	bool unavailableDoorRejectsTraversal()
	{
		core::Building building("Unavailable door", 6, 2);
		auto fore = building.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 5, 1);
		auto back = building.addRoom("Back", CORE_LAYER_BACK, 0, 0, 5, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Unavailable;
		building.addSectorDoor(0, 2, options);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = building.createAgent("Rejected traveller", fore, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& building.getSimulationSnapshot().traversalRequests.empty(); ++i)
		{
			building.advanceTick();
		}
		auto snapshot = building.getSimulationSnapshot();
		return agent->getSector() == building.getSector(fore).get()
			&& agent->getState() == core::Agent::State::WaitingForTraversal
			&& snapshot.traversalRequests.size() == 1
			&& snapshot.traversalRequests.front().state == core::TraversalRequestState::Denied
			&& snapshot.deviceOperations.empty() && snapshot.traversalPermits.empty();
	}

	bool interactionBindingAggregationIsMeaningful()
	{
		core::Building building("Binding aggregation", 3, 2);
		auto corridorIndex = building.addCorridor(0, 0, 2);
		building.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto actor = building.createAgent("Binding operator", corridorIndex, 0, 0.5f);

		core::InteractionBinding required{ { core::DeviceCommandType::SetSectorLights, sectorId, false },
			core::InteractionBindingRequirement::Required };
		core::InteractionBinding bestEffort{ { core::DeviceCommandType::SetSectorLights, sectorId, true },
			core::InteractionBindingRequirement::BestEffort };
		auto point = building.createInteractionPoint("Multi-binding control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { required, bestEffort });
		auto requestId = building.requestInteraction(point, actor);
		auto request = building.lookupInteractionRequest(requestId);
		if (!request || request.entity->getOperations().size() != 2)
		{
			return false;
		}
		auto failedBestEffort = request.entity->getOperations()[1].first;
		building.lookupDeviceOperation(failedBestEffort).entity->setState(core::DeviceOperationState::Failed);
		building.advanceTicks(4);
		request = building.lookupInteractionRequest(requestId);
		if (!request || request.entity->getResult() != core::InteractionResult::SucceededWithBestEffortFailure)
		{
			return false;
		}

		core::InteractionBinding failingRequired{ { core::DeviceCommandType::SetSectorLights, sectorId, true },
			core::InteractionBindingRequirement::Required };
		auto requiredPoint = building.createInteractionPoint("Required control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { failingRequired });
		auto failedRequestId = building.requestInteraction(requiredPoint, actor);
		auto failedRequest = building.lookupInteractionRequest(failedRequestId);
		if (!failedRequest)
		{
			return false;
		}
		building.lookupDeviceOperation(failedRequest.entity->getOperations().front().first).entity->setState(
			core::DeviceOperationState::Failed);
		building.advanceTick();
		return building.lookupInteractionRequest(failedRequestId).entity->getResult() == core::InteractionResult::Failed;
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
		if (!ordinaryTraversalCommitsOnlyAtDestination())
		{
			std::cerr << "FAIL: ordinary traversal did not hold a permit through atomic commit\n";
			return 1;
		}
		if (!deniedTraversalCannotBeCrossed())
		{
			std::cerr << "FAIL: denied traversal was crossed or leaked its request\n";
			return 1;
		}
		if (!cancellationReleasesPermitWithoutCommitting())
		{
			std::cerr << "FAIL: traversal cancellation leaked or committed membership\n";
			return 1;
		}
		if (!typedLightingInteractionCoalescesAndCancelsByRequester())
		{
			std::cerr << "FAIL: typed lighting interaction, coalescing, or requester cancellation failed\n";
			return 1;
		}
		if (!interactionBindingAggregationIsMeaningful())
		{
			std::cerr << "FAIL: required and best-effort interaction aggregation failed\n";
			return 1;
		}
		if (!singleAgentDoorJourney(core::DoorActivationMode::Manual))
		{
			std::cerr << "FAIL: manual door journey or hold-open safety failed\n";
			return 1;
		}
		if (!singleAgentDoorJourney(core::DoorActivationMode::Automatic))
		{
			std::cerr << "FAIL: automatic door journey or hold-open safety failed\n";
			return 1;
		}
		if (!remoteDoorUsesOnePhysicalOperatorAndSharedOperation())
		{
			std::cerr << "FAIL: remote door did not share physical preparation or survive operator cancellation\n";
			return 1;
		}
		if (!remoteDoorWithoutReachableControlIsUnavailable())
		{
			std::cerr << "FAIL: remote door without a reachable control was not reported unavailable\n";
			return 1;
		}
		if (!fairDoorQueuesServeBothSidesInStableOrder())
		{
			std::cerr << "FAIL: two-sided door queues were not separated, FIFO, or fair\n";
			return 1;
		}
		if (!queuedCancellationReleasesAndAdvancesPositions())
		{
			std::cerr << "FAIL: queued cancellation leaked a ticket or physical position\n";
			return 1;
		}
		if (!wideDoorLanesAndGracefulDisableAreSafe())
		{
			std::cerr << "FAIL: wide door lanes exceeded capacity or deactivation was unsafe\n";
			return 1;
		}
		if (!resilientWaitingRetainsPriorityAndExpiresPermits())
		{
			std::cerr << "FAIL: resilient waiting lost priority, leaked reservations, or failed permit expiry\n";
			return 1;
		}
		if (!doorLeasesAndSensorObservationsPreventUnsafeClosure())
		{
			std::cerr << "FAIL: door leases or sensor observations allowed unsafe closure\n";
			return 1;
		}
		if (!unavailableDoorRejectsTraversal())
		{
			std::cerr << "FAIL: unavailable door did not reject traversal\n";
			return 1;
		}
		if (!finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed())
		{
			std::cerr << "FAIL: finite ladder capacity, reservations, cancellation, or climb speed failed\n";
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
