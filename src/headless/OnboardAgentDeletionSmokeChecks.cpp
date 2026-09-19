// Ticket #57: deleting an Agent which is riding a Lift must not leak its slot.
//
// The editor deletes a selected Agent by calling clearPath() and then
// Building::removeAgent(). clearPath() cancels the route - which for an onboard
// Agent means "ask for a safe transport exit" - and leaves the Agent Idle. Idle is
// what removeAgent() has always accepted, so the Agent entity vanished while the
// Lift's manifest still owned its handle. The safe-exit machinery then dropped the
// missing passenger from its pending-exit set without ever clearing the manifest
// slot, and the capacity was consumed forever.
//
// What is pinned down here:
//
//   delete a rider out of a moving Lift   -> the Agent is gone AND every
//                                          capacity, stop-request, safe-exit,
//                                          request, permit and lease handle is
//                                          gone with it
//   the released slot                     -> the next passenger boards and rides
//   a queued (not yet boarded) Agent      -> still deletable, nothing leaks
//   the Lift itself                       -> keeps serving its remaining stops

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Agent.h"
#include "core/Building.h"
#include "core/Simulation.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	constexpr uint64_t DrainTicks = 1000;

	// A two-stop, capacity-one Lift between two Corridors: the smallest building
	// where a passenger can be genuinely onboard a moving car.
	struct LiftScenario
	{
		core::Building building;
		uint32_t lower{ 0 };
		uint32_t upper{ 0 };
		core::TraversalResourceId lift{};

		explicit LiftScenario(std::string const& name)
			: building(name, 8, 6)
		{
			lower = building.addCorridor(0, 0, 7);
			upper = building.addCorridor(2, 0, 7);
			core::Building::CreateLiftOptions options;
			options.cellsWide = 1;
			options.stopOffsets = { 0, 2 };
			options.capacity = 1;
			auto created = building.addLift(1, 0, 2, options);
			lift = created.traversalResource;
			building.finishBuild();
		}

		core::TraversalResourceSnapshot const& liftSnapshot(
			core::SimulationSnapshot const& snapshot) const
		{
			auto found = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(),
				[&](core::TraversalResourceSnapshot const& resource)
				{ return resource.id == lift; });
			require(found != snapshot.traversalResources.end(),
				"The Lift coordinator disappeared from the snapshot");
			require(found->isLift, "The Lift coordinator is no longer a Lift");
			return *found;
		}

		core::AgentId boardPassenger(std::string const& name)
		{
			auto target = building.getGraph()->getClosestVertexInSector(
				building.getSector(upper).get(), { 2.5f, 2.0f });
			require(target != nullptr, "No route target in the upper Corridor");
			auto passengerId = building.createAgent(name, lower, 0, 0.5f);
			auto passenger = building.lookupAgent(passengerId).entity;
			require(passenger != nullptr, "The passenger was not created");
			auto path = building.getGraph()->calculatePath(passenger, target);
			require(path != nullptr, "No route was found for the passenger");
			passenger->setPath(path, true);
			return passengerId;
		}

		// Advances until the car is moving with exactly one occupant, which is the
		// moment the editor's Delete would strand a handle.
		void advanceUntilMovingWithOccupant(core::AgentId passengerId)
		{
			for (uint64_t tick = 0; tick < DrainTicks; ++tick)
			{
				building.advanceTick();
				auto const snapshot = building.getSimulationSnapshot();
				auto const& liftState = liftSnapshot(snapshot);
				if (liftState.liftMoving && liftState.occupantCount == 1
					&& liftState.liftPassenger == passengerId)
					return;
			}
			throw std::runtime_error("The Lift never carried the passenger while moving");
		}
	};

	// Every structure which can name an Agent, checked through the snapshot. These
	// are identity checks only: aggregate counts a still-living Agent could
	// legitimately cause are asserted by the individual scenarios.
	void requireNoStaleHandles(core::SimulationSnapshot const& snapshot,
		core::AgentId removed, std::string const& what)
	{
		auto stale = std::string{};
		auto check = [&](bool held, char const* where)
		{
			if (!held) return;
			stale += where;
			stale += "; ";
		};

		for (auto const& resource : snapshot.traversalResources)
		{
			check(resource.liftPassenger == removed, "liftPassenger alias");
			check(std::any_of(resource.capacityPositions.begin(),
				resource.capacityPositions.end(),
				[&](core::CapacityPositionSnapshot const& position)
				{ return position.occupant == removed; }), "capacity manifest");
			check(std::any_of(resource.liftAgents.begin(), resource.liftAgents.end(),
				[&](core::LiftAgentSnapshot const& rider)
				{ return rider.agent == removed; }), "lift rider roster");
		}
		check(std::any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
			[&](core::TraversalRequestSnapshot const& request)
			{ return request.owner == removed; }), "traversal requests");
		check(std::any_of(snapshot.traversalPermits.begin(), snapshot.traversalPermits.end(),
			[&](core::TraversalPermitSnapshot const& permit)
			{ return permit.owner == removed; }), "traversal permits");
		// Only live claims matter. A completed CallLift operation or a succeeded
		// button press is a historical record, not capacity the Agent still holds.
		check(std::any_of(snapshot.interactionRequests.begin(), snapshot.interactionRequests.end(),
			[&](core::InteractionRequestSnapshot const& request)
			{ return request.actor == removed
				&& request.result == core::InteractionResult::Pending; }),
			"live interaction requests");
		check(std::any_of(snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(),
			[&](core::DeviceOperationSnapshot const& operation)
			{ return (operation.requester == removed
					|| std::find(operation.requesters.begin(), operation.requesters.end(), removed)
						!= operation.requesters.end())
				&& (operation.state == core::DeviceOperationState::Pending
					|| operation.state == core::DeviceOperationState::Running); }),
			"live device operations");
		check(std::any_of(snapshot.agents.begin(), snapshot.agents.end(),
			[&](core::AgentSnapshot const& agent) { return agent.id == removed; }),
			"agent registry");

		require(stale.empty(), std::format("{} left a stale handle: {}", what, stale).c_str());
	}

	// The ticket's reproduction: delete the passenger while the car is in flight.
	void deletingARiderOfAMovingLiftFreesItsSlot()
	{
		LiftScenario scenario("Delete rider mid-flight");
		auto const passengerId = scenario.boardPassenger("Rider");
		scenario.advanceUntilMovingWithOccupant(passengerId);

		// Exactly what UI.cpp does for Delete and for a clipboard cut.
		auto passenger = scenario.building.lookupAgent(passengerId).entity;
		require(passenger != nullptr, "The rider vanished before the delete");
		passenger->clearPath();
		auto const removal = scenario.building.removeAgent(passengerId);
		require(removal.removed, std::format(
			"Deleting an onboard Agent was refused: {}", removal.diagnostic).c_str());
		require(!scenario.building.lookupAgent(passengerId),
			"The Agent entity survived removeAgent()");

		for (uint64_t tick = 0; tick < DrainTicks; ++tick)
		{
			scenario.building.advanceTick();
		}

		auto const snapshot = scenario.building.getSimulationSnapshot();
		auto const& liftState = scenario.liftSnapshot(snapshot);
		require(liftState.occupantCount == 0, std::format(
			"The Lift still reports {} occupant(s) after its rider was deleted",
			liftState.occupantCount).c_str());
		require(!liftState.liftPassenger, "The Lift still names the deleted Agent as its passenger");
		require(liftState.liftPendingSafeExits == 0, "The Lift still awaits a safe exit for nobody");
		require(std::all_of(liftState.liftStopRequestOwnerCounts.begin(),
			liftState.liftStopRequestOwnerCounts.end(),
			[](uint32_t count) { return count == 0; }),
			"The deleted Agent still holds a stop request");
		requireNoStaleHandles(snapshot, passengerId, "Deleting a rider from a moving Lift");
	}

	// The slot is not merely empty on paper: the next passenger really can use it.
	void theReleasedSlotCarriesTheNextPassenger()
	{
		LiftScenario scenario("Reuse released slot");
		auto const firstId = scenario.boardPassenger("First rider");
		scenario.advanceUntilMovingWithOccupant(firstId);

		auto first = scenario.building.lookupAgent(firstId).entity;
		require(first != nullptr, "The first rider vanished before the delete");
		first->clearPath();
		require(scenario.building.removeAgent(firstId).removed,
			"Deleting the first rider was refused");

		auto const secondId = scenario.boardPassenger("Second rider");
		for (uint64_t tick = 0; tick < DrainTicks * 4; ++tick)
		{
			scenario.building.advanceTick();
			auto const* second = scenario.building.lookupAgent(secondId).entity;
			if (second && second->getSector() == scenario.building.getSector(scenario.upper).get()
				&& second->getState() == core::Agent::State::Idle)
			{
				auto const snapshot = scenario.building.getSimulationSnapshot();
				requireNoStaleHandles(snapshot, firstId, "Reusing the released slot");
				return;
			}
		}
		throw std::runtime_error("The second rider could not use the slot the delete released");
	}

	// A queued Agent has no manifest slot yet; deleting it must stay ordinary and
	// must not leave the queue or its admission reservation behind either.
	void deletingAQueuedWaiterLeavesTheQueueClean()
	{
		LiftScenario scenario("Delete queued waiter");
		auto const riderId = scenario.boardPassenger("Rider");
		scenario.advanceUntilMovingWithOccupant(riderId);

		auto const waiterId = scenario.boardPassenger("Queued waiter");
		auto waiter = scenario.building.lookupAgent(waiterId).entity;
		require(waiter != nullptr, "The waiter was not created");

		// Let the waiter reach the Lift's door queue.
		for (uint64_t tick = 0; tick < DrainTicks; ++tick)
		{
			scenario.building.advanceTick();
			auto const snapshot = scenario.building.getSimulationSnapshot();
			auto const held = std::any_of(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](core::TraversalRequestSnapshot const& request)
				{ return request.owner == waiterId; });
			if (!held) continue;

			waiter->clearPath();
			require(scenario.building.removeAgent(waiterId).removed,
				"Deleting a queued waiter was refused");
			for (uint64_t drained = 0; drained < DrainTicks; ++drained)
				scenario.building.advanceTick();
			requireNoStaleHandles(scenario.building.getSimulationSnapshot(), waiterId,
				"Deleting a queued waiter");
			return;
		}
		throw std::runtime_error("The second Agent never queued for the Lift");
	}

	// Deleting the rider must not strand the car: it lands, and a later passenger
	// can still call it from either floor.
	void theLiftKeepsServingAfterItsRiderIsDeleted()
	{
		LiftScenario scenario("Lift keeps serving");
		auto const riderId = scenario.boardPassenger("Rider");
		scenario.advanceUntilMovingWithOccupant(riderId);

		auto rider = scenario.building.lookupAgent(riderId).entity;
		require(rider != nullptr, "The rider vanished before the delete");
		rider->clearPath();
		require(scenario.building.removeAgent(riderId).removed,
			"Deleting the rider was refused");

		for (uint64_t tick = 0; tick < DrainTicks; ++tick)
		{
			scenario.building.advanceTick();
			auto const snapshot = scenario.building.getSimulationSnapshot();
			auto const& liftState = scenario.liftSnapshot(snapshot);
			require(liftState.enabled, "The Lift disabled itself after losing its rider");
			if (!liftState.liftMoving && liftState.occupantCount == 0
				&& liftState.liftPendingSafeExits == 0)
				return;
		}
		throw std::runtime_error("The Lift never came to rest after its rider was deleted");
	}
	// A climber holds an extension occupant lease alongside its capacity slot.
	// Deleting one mid-climb has to surrender both, or the extensible resource
	// stays extended for somebody who no longer exists.
	void deletingAClimberReleasesTheExtensionLease()
	{
		core::Building building("Delete climber", 4, 4);
		auto const lower = building.addCorridor(0, 0, 3);
		auto const upper = building.addCorridor(2, 0, 3);
		core::Building::CreateLadderOptions options{ 3, true, false };
		auto const created = building.addLadder(1, 0, 1, options);
		building.finishBuild();

		auto target = building.getGraph()->getClosestVertexInSector(
			building.getSector(upper).get(), { 1.5f, 2.0f });
		require(target != nullptr, "No route target above the ladder");
		auto const first = building.createAgent("Climber one", lower, 0, 1.5f);
		auto const second = building.createAgent("Climber two", lower, 0, 1.5f);
		for (auto id : { first, second })
		{
			auto agent = building.lookupAgent(id).entity;
			require(agent != nullptr, "A climber was not created");
			auto path = building.getGraph()->calculatePath(agent, target);
			require(path != nullptr, "No route was found for a climber");
			agent->setPath(path, true);
		}

		auto onCapacity = [&](core::AgentId id, core::TraversalResourceSnapshot const& resource)
		{
			auto const* agent = building.lookupAgent(id).entity;
			return agent && agent->getSector()
				&& core::SectorId{ (uint64_t)agent->getSector()->getIndex() + 1 } == resource.capacitySector;
		};

		core::AgentId deleted{};
		for (uint64_t tick = 0; tick < 1000 && !deleted; ++tick)
		{
			building.advanceTick();
			auto const snapshot = building.getSimulationSnapshot();
			auto const resource = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(),
				[&](core::TraversalResourceSnapshot const& value)
				{ return value.id == created.traversalResource; });
			require(resource != snapshot.traversalResources.end() && resource->isExtensible,
				"The extensible ladder vanished from the snapshot");
			if (resource->extensionOccupantLeaseCount == 0) continue;
			for (auto id : { first, second })
			{
				if (!onCapacity(id, *resource)) continue;
				auto agent = building.lookupAgent(id).entity;
				agent->clearPath();
				require(building.removeAgent(id).removed,
					"Deleting a climber from the ladder was refused");
				deleted = id;
				break;
			}
		}
		require(static_cast<bool>(deleted), "No climber ever occupied the ladder");

		auto const survivorId = first == deleted ? second : first;
		for (uint64_t tick = 0; tick < 4000; ++tick)
		{
			building.advanceTick();
			auto const* survivor = building.lookupAgent(survivorId).entity;
			if (survivor && survivor->getState() == core::Agent::State::Idle
				&& survivor->getSector() == building.getSector(upper).get())
				break;
		}

		auto const snapshot = building.getSimulationSnapshot();
		auto const resource = std::find_if(snapshot.traversalResources.begin(),
			snapshot.traversalResources.end(),
			[&](core::TraversalResourceSnapshot const& value)
			{ return value.id == created.traversalResource; });
		require(resource != snapshot.traversalResources.end(), "The ladder vanished");
		auto const* survivor = building.lookupAgent(survivorId).entity;
		require(survivor && survivor->getSector() == building.getSector(upper).get(),
			"The surviving climber could not finish after its companion was deleted");
		require(std::none_of(resource->capacityPositions.begin(),
			resource->capacityPositions.end(),
			[&](core::CapacityPositionSnapshot const& position)
			{ return position.occupant == deleted; }),
			"The deleted climber still holds a ladder slot");
		requireNoStaleHandles(snapshot, deleted, "Deleting a climber");
	}
}

void runOnboardAgentDeletionSmokeChecks()
{
	deletingARiderOfAMovingLiftFreesItsSlot();
	theReleasedSlotCarriesTheNextPassenger();
	deletingAQueuedWaiterLeavesTheQueueClean();
	theLiftKeepsServingAfterItsRiderIsDeleted();
	deletingAClimberReleasesTheExtensionLease();
}
