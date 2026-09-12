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
		if (!unavailableDoorRejectsTraversal())
		{
			std::cerr << "FAIL: unavailable door did not reject traversal\n";
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
