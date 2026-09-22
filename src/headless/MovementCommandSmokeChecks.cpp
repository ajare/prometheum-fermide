#include <algorithm>
#include <stdexcept>
#include <vector>
#include "core/Agent.h"
#include "core/Building.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	void ordinaryCommands()
	{
		core::Building building("Movement commands", 12, 2);
		auto room = building.addRoom("Room", 0, 0, 0, 10, 1);
		auto isolated = building.addRoom("Isolated", 0, 0, 10, 2, 1);
		building.addSectorMarker(room, 0, 8.5f, "End");
		building.addSectorMarker(room, 0, 2.5f, "Other");
		building.addSectorMarker(isolated, 0, 0.5f, "Unreachable");
		building.finishBuild();
		auto markers = building.getMarkerIds();
		auto id = building.createAgent("Walker", room, 0, 0.5f);
		using Status = core::MovementCommandStatus;
		require(building.moveAgentToMarker({}, markers[0]).status == Status::UnknownAgent, "Unknown Agent accepted");
		require(building.moveAgentToMarker(id, {}).status == Status::UnknownMarker, "Unknown Marker accepted");
		building.pauseSimulation();
		require(building.setAgentActive(id, false), "Deactivation refused");
		require(building.moveAgentToMarker(id, markers[0]).status == Status::InactiveAgent, "Inactive Agent accepted");
		require(building.setAgentActive(id, true), "Activation refused");
		building.resumeSimulation();
		require(building.moveAgentToMarker(id, markers[0]).accepted(), "Movement refused");
		require(building.moveAgentToMarker(id, markers[0]).status == Status::NoOp, "Same destination not idempotent");
		require(building.moveAgentToMarker(id, markers[1]).status == Status::AgentBusy, "Busy Agent replaced");
		building.advanceTicks(5);
		building.consumeSimulationEvents();
		require(building.cancelAgentMovement(id).accepted(), "Cancellation refused");
		require(building.consumeSimulationEvents().empty(), "Cancellation completed synchronously");
		building.advanceTick();
		auto events = building.consumeSimulationEvents();
		unsigned cancelled = 0;
		for (auto const& event : events) cancelled += event.type == core::SimulationEventType::MovementCancelled;
		require(cancelled == 1, "Cancellation outcome missing or duplicated");
		require(building.cancelAgentMovement(id).status == Status::NoOp, "Idle cancellation not idempotent");
		require(building.moveAgentToMarker(id, markers[1]).accepted(), "Replacement after cancellation refused");
		building.advanceTicks(1000);
		unsigned reached = 0;
		for (auto const& event : building.consumeSimulationEvents()) reached += event.type == core::SimulationEventType::DestinationReached;
		require(reached == 1, "Walking destination outcome missing");
		require(building.moveAgentToMarker(id, markers[1]).accepted(), "Already-at-destination command refused");
		building.advanceTicks(5);
		reached = 0;
		for (auto const& event : building.consumeSimulationEvents()) reached += event.type == core::SimulationEventType::DestinationReached;
		require(reached == 1, "Already-at-destination did not complete");
		require(building.moveAgentToMarker(id, markers[0]).accepted(), "Replanning fixture movement refused");
		building.advanceTicks(5);
		building.pauseSimulation();
		building.finishBuild();
		building.resumeSimulation();
		building.advanceTicks(1000);
		reached = 0;
		for (auto const& event : building.consumeSimulationEvents())
		{
			require(event.type != core::SimulationEventType::RouteLost, "Successful same-destination replan reported route loss");
			reached += event.type == core::SimulationEventType::DestinationReached;
		}
		require(reached == 1, "Same-destination replanning lost the movement goal");
		require(building.moveAgentToMarker(id, markers[2]).accepted(), "Unreachable destination was not accepted as intent");
		building.advanceTick();
		unsigned lost = 0;
		for (auto const& event : building.consumeSimulationEvents()) lost += event.type == core::SimulationEventType::RouteLost;
		require(lost == 1 && building.getSimulationSnapshot().traversalRequests.empty(), "Initial route loss leaked claims or outcome");
	}

	void cancelDoorCrossing()
	{
		core::Building building("Door cancellation", 8, 2);
		auto front = building.addRoom("Front", 0, 0, 0, 8, 1);
		auto back = building.addRoom("Back", 1, 0, 0, 8, 1);
		building.addSectorDoor(front, 0, 2, {});
		building.addSectorMarker(back, 0, 6.5f, "End");
		building.finishBuild();
		auto id = building.createAgent("Walker", front, 0, 0.5f);
		auto agent = building.lookupAgent(id).entity;
		auto marker = building.getMarkerIds()[0];
		require(building.moveAgentToMarker(id, marker).accepted(), "Door route refused");
		bool requested = false, completed = false;
		for (unsigned tick = 0; tick < 2000 && !completed; ++tick)
		{
			building.advanceTick();
			for (auto const& event : building.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::MovementCancelled) completed = true;
			if (!requested && agent->getState() == core::Agent::State::TraversingEdge)
				for (auto const& request : building.getSimulationSnapshot().traversalRequests)
					if (request.owner == id && request.edgeType == core::EdgeType::Door && request.permit)
					{
						require(building.cancelAgentMovement(id).accepted(), "Door cancellation refused");
						requested = true;
					}
		}
		require(completed && agent->getSector()->getIndex() == back, "Door crossing was interrupted before commit");
		auto snapshot = building.getSimulationSnapshot();
		require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Door claims leaked");
		require(building.moveAgentToMarker(id, marker).accepted(), "Movement after Door cancellation refused");
		building.advanceTicks(1000);
		unsigned reached = 0;
		for (auto const& event : building.consumeSimulationEvents()) reached += event.type == core::SimulationEventType::DestinationReached;
		require(reached == 1, "Door journey did not reach Marker");
	}

	uint64_t cancelLiftJourney(unsigned boundary)
	{
		core::Building building("Cancellation boundaries", 6, 4);
		auto lower = building.addCorridor(0, 0, 5);
		auto upper = building.addCorridor(2, 0, 5);
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		auto lift = building.addLift(1, 0, 2, options);
		building.addSectorMarker(upper, 0, 4.5f, "Destination");
		building.finishBuild();
		auto id = building.createAgent("Passenger", lower, 0, 0.5f);
		auto agent = building.lookupAgent(id).entity;
		require(building.moveAgentToMarker(id, building.getMarkerIds()[0]).accepted(), "Lift movement refused");
		bool requested = false;
		uint64_t completed = 0;
		for (unsigned tick = 0; tick < 15000; ++tick)
		{
			building.advanceTick();
			for (auto const& event : building.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::MovementCancelled)
				{
					require(!completed, "Duplicate cancellation completion");
					completed = event.tick;
				}
			if (completed)
			{
				auto snapshot = building.getSimulationSnapshot();
				require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Cancellation leaked traversal transactions");
				for (auto const& resource : snapshot.traversalResources)
					require(resource.occupantCount == 0 && resource.admissionReservationCount == 0, "Cancellation leaked capacity");
				for (auto const& request : snapshot.interactionRequests)
					require(request.actor != id || request.result != core::InteractionResult::Pending, "Cancellation leaked interaction");
				for (auto const& operation : snapshot.deviceOperations)
					require(std::find(operation.requesters.begin(), operation.requesters.end(), id) == operation.requesters.end(), "Cancellation leaked device-operation claim");
				require(agent->getSector()->getIndex() != lift.lift.sector->getIndex(), "Cancellation stranded passenger");
				building.advanceTicks(10);
				for (auto const& event : building.consumeSimulationEvents())
					require(event.type != core::SimulationEventType::MovementCancelled, "Repeated cancellation outcome");
				return completed;
			}
			bool queued = false, permitted = false, crossing = false, riding = false;
			auto snapshot = building.getSimulationSnapshot();
			for (auto const& request : snapshot.traversalRequests)
				if (request.owner == id && request.edgeType == core::EdgeType::Door)
				{
					queued = bool(request.queueTicket) && !request.permit;
					permitted = bool(request.permit);
					crossing = permitted && agent->getState() == core::Agent::State::TraversingEdge;
				}
			for (auto const& resource : snapshot.traversalResources)
				if (resource.id == lift.traversalResource)
					riding = resource.liftMoving && agent->getSector()->getIndex() == lift.lift.sector->getIndex();
			bool atBoundary = boundary == 0 ? queued : boundary == 1 ? permitted : boundary == 2 ? crossing : riding;
			if (!requested && atBoundary)
			{
				require(building.cancelAgentMovement(id).accepted(), "Boundary cancellation refused");
				requested = true;
			}
		}
		throw std::runtime_error("Lift cancellation did not complete");
	}
}

void runMovementCommandSmokeChecks()
{
	ordinaryCommands();
	cancelDoorCrossing();
	for (unsigned boundary = 0; boundary < 4; ++boundary)
		require(cancelLiftJourney(boundary) == cancelLiftJourney(boundary), "Cancellation was not deterministic");
}
