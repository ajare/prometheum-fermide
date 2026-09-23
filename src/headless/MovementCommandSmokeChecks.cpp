#include <algorithm>
#include <stdexcept>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	void ordinaryCommands()
	{
		core::World world("Movement commands", 12, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 10, 1);
		auto isolated = world.addRoom("Isolated", 0, 0, 10, 2, 1);
		world.addSectorMarker(room, 0, 8.5f, "End");
		world.addSectorMarker(room, 0, 2.5f, "Other");
		world.addSectorMarker(isolated, 0, 0.5f, "Unreachable");
		world.finishBuild();
		auto markers = world.getMarkerIds();
		auto id = world.createAgent("Walker", room, 0, 0.5f);
		using Status = core::MovementCommandStatus;
		require(world.moveAgentToMarker({}, markers[0]).status == Status::UnknownAgent, "Unknown Agent accepted");
		require(world.moveAgentToMarker(id, {}).status == Status::UnknownMarker, "Unknown Marker accepted");
		world.pauseSimulation();
		require(world.setAgentActive(id, false), "Deactivation refused");
		require(world.moveAgentToMarker(id, markers[0]).status == Status::InactiveAgent, "Inactive Agent accepted");
		require(world.setAgentActive(id, true), "Activation refused");
		world.resumeSimulation();
		require(world.moveAgentToMarker(id, markers[0]).accepted(), "Movement refused");
		require(world.moveAgentToMarker(id, markers[0]).status == Status::NoOp, "Same destination not idempotent");
		require(world.moveAgentToMarker(id, markers[1]).status == Status::AgentBusy, "Busy Agent replaced");
		world.advanceTicks(5);
		world.consumeSimulationEvents();
		require(world.cancelAgentMovement(id).accepted(), "Cancellation refused");
		require(world.consumeSimulationEvents().empty(), "Cancellation completed synchronously");
		world.advanceTick();
		auto events = world.consumeSimulationEvents();
		unsigned cancelled = 0;
		for (auto const& event : events)
			if (event.type == core::SimulationEventType::MovementCancelled)
			{
				++cancelled;
				require(event.destinationMarker == markers[0]
					&& event.movementCancellationReason
						== core::MovementCancellationReason::Explicit
					&& event.tick != 0 && event.sequence != 0,
					"Cancellation event lacked immutable semantic payload");
			}
		require(cancelled == 1, "Cancellation outcome missing or duplicated");
		require(world.cancelAgentMovement(id).status == Status::NoOp, "Idle cancellation not idempotent");
		require(world.moveAgentToMarker(id, markers[1]).accepted(), "Replacement after cancellation refused");
		world.advanceTicks(1000);
		unsigned reached = 0;
		for (auto const& event : world.consumeSimulationEvents()) reached += event.type == core::SimulationEventType::DestinationReached;
		require(reached == 1, "Walking destination outcome missing");
		require(world.moveAgentToMarker(id, markers[1]).accepted(), "Already-at-destination command refused");
		world.advanceTicks(5);
		reached = 0;
		for (auto const& event : world.consumeSimulationEvents()) reached += event.type == core::SimulationEventType::DestinationReached;
		require(reached == 1, "Already-at-destination did not complete");
		require(world.moveAgentToMarker(id, markers[0]).accepted(), "Replanning fixture movement refused");
		world.advanceTicks(5);
		world.pauseSimulation();
		world.finishBuild();
		world.resumeSimulation();
		world.advanceTicks(1000);
		reached = 0;
		for (auto const& event : world.consumeSimulationEvents())
		{
			require(event.type != core::SimulationEventType::RouteLost, "Successful same-destination replan reported route loss");
			reached += event.type == core::SimulationEventType::DestinationReached;
		}
		require(reached == 1, "Same-destination replanning lost the movement goal");
		require(world.moveAgentToMarker(id, markers[2]).accepted(), "Unreachable destination was not accepted as intent");
		world.advanceTick();
		unsigned lost = 0;
		for (auto const& event : world.consumeSimulationEvents()) lost += event.type == core::SimulationEventType::RouteLost;
		require(lost == 1 && world.getSimulationSnapshot().traversalRequests.empty(), "Initial route loss leaked claims or outcome");
	}

	void cancelDoorCrossing()
	{
		core::World world("Door cancellation", 8, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 8, 1);
		world.addSectorDoor(front, 0, 2, {});
		world.addSectorMarker(back, 0, 6.5f, "End");
		world.finishBuild();
		auto id = world.createAgent("Walker", front, 0, 0.5f);
		auto agent = world.lookupAgent(id).entity;
		auto marker = world.getMarkerIds()[0];
		require(world.moveAgentToMarker(id, marker).accepted(), "Door route refused");
		bool requested = false, completed = false;
		for (unsigned tick = 0; tick < 2000 && !completed; ++tick)
		{
			world.advanceTick();
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::MovementCancelled) completed = true;
			if (!requested && agent->getState() == core::Agent::State::TraversingEdge)
				for (auto const& request : world.getSimulationSnapshot().traversalRequests)
					if (request.owner == id && request.edgeType == core::EdgeType::Door && request.permit)
					{
						require(world.cancelAgentMovement(id).accepted(), "Door cancellation refused");
						requested = true;
					}
		}
		require(completed && agent->getSector()->getIndex() == back, "Door crossing was interrupted before commit");
		auto snapshot = world.getSimulationSnapshot();
		require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Door claims leaked");
		require(world.moveAgentToMarker(id, marker).accepted(), "Movement after Door cancellation refused");
		world.advanceTicks(1000);
		unsigned reached = 0;
		for (auto const& event : world.consumeSimulationEvents()) reached += event.type == core::SimulationEventType::DestinationReached;
		require(reached == 1, "Door journey did not reach Marker");
	}

	uint64_t cancelLiftJourney(unsigned boundary)
	{
		core::World world("Cancellation boundaries", 6, 4);
		auto lower = world.addCorridor(0, 0, 5);
		auto upper = world.addCorridor(2, 0, 5);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		auto lift = world.addLift(1, 0, 2, options);
		world.addSectorMarker(upper, 0, 4.5f, "Destination");
		world.finishBuild();
		auto id = world.createAgent("Passenger", lower, 0, 0.5f);
		auto agent = world.lookupAgent(id).entity;
		require(world.moveAgentToMarker(id, world.getMarkerIds()[0]).accepted(), "Lift movement refused");
		bool requested = false;
		uint64_t completed = 0;
		for (unsigned tick = 0; tick < 15000; ++tick)
		{
			world.advanceTick();
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::MovementCancelled)
				{
					require(!completed, "Duplicate cancellation completion");
					completed = event.tick;
				}
			if (completed)
			{
				auto snapshot = world.getSimulationSnapshot();
				require(snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty(), "Cancellation leaked traversal transactions");
				for (auto const& resource : snapshot.traversalResources)
					require(resource.occupantCount == 0 && resource.admissionReservationCount == 0, "Cancellation leaked capacity");
				for (auto const& request : snapshot.interactionRequests)
					require(request.actor != id || request.result != core::InteractionResult::Pending, "Cancellation leaked interaction");
				for (auto const& operation : snapshot.deviceOperations)
					require(std::find(operation.requesters.begin(), operation.requesters.end(), id) == operation.requesters.end(), "Cancellation leaked device-operation claim");
				require(agent->getSector()->getIndex() != lift.lift.sector->getIndex(), "Cancellation stranded passenger");
				world.advanceTicks(10);
				for (auto const& event : world.consumeSimulationEvents())
					require(event.type != core::SimulationEventType::MovementCancelled, "Repeated cancellation outcome");
				return completed;
			}
			bool queued = false, permitted = false, crossing = false, riding = false;
			auto snapshot = world.getSimulationSnapshot();
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
				require(world.cancelAgentMovement(id).accepted(), "Boundary cancellation refused");
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
