#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/Graph.h"
#include "core/MarkerSectorObject.h"
#include "core/Path.h"
#include "core/Vertex.h"
#include "core/Building.h"
#include "core/Coordination.h"
#include "core/Exceptions.h"
#include "core/ExtensibleObject.h"
#include "core/Sector.h"
#include "core/Simulation.h"


namespace core
{

	using namespace std;

	// Agent lifecycle moved out of Building (ADR 0004 stage 1). The behaviour is
	// unchanged: the coordinator works on Building's registries through
	// friendship, and calls back through the Building facade for the machinery
	// which has not moved out of Building yet - the modified-state marker,
	// sector lookup, interaction cancellation, and device-operation
	// cancellation and removal. The Agent snapshots it publishes are built by
	// the coordinator's own snapshot seam, which joined it in stage 5. The stop
	// requests it drops for a departing passenger go to the coordinator's own
	// lift scheduling helpers, and traversal cancellation and release to the
	// transaction lifecycle which joined it in stage 4.

	AgentId SimulationCoordinator::addOwnedAgentToSector(unique_ptr<Agent> agent, uint32_t sectorId, uint32_t deckOffset, float xOffset)
	{
		if (!agent)
		{
			throw invalid_argument("Building cannot own a null Agent");
		}
		if (mBuilding.mAgentIds.contains(agent.get()))
		{
			throw invalid_argument("Agent is already owned by this Building");
		}

		auto sector = mBuilding._getSector(sectorId);
		if (sector->getType() == SectorType::Background)
		{
			throw BuildingException(&mBuilding,
				"An Agent cannot occupy a Background: it owns no walkable floor and takes no part in traversal");
		}
		auto rawAgent = agent.get();
		rawAgent->attachToBuilding(&mBuilding);
		sector->enterAgent(rawAgent, deckOffset, xOffset);
		auto id = mBuilding.mAgents.add(std::move(agent));
		mBuilding.mAgentIds.emplace(rawAgent, id);

		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::AgentAdded;
		event.agent = makeAgentSnapshot(rawAgent);
		mBuilding.mEvents.push_back(std::move(event));
		return id;
	}

	AgentId SimulationCoordinator::addOwnedAgentToSector(unique_ptr<Agent> agent, uint32_t sectorId)
	{
		if (!agent)
		{
			throw invalid_argument("Building cannot own a null Agent");
		}
		if (mBuilding.mAgentIds.contains(agent.get()))
		{
			throw invalid_argument("Agent is already owned by this Building");
		}

		auto sector = mBuilding._getSector(sectorId);
		if (sector->getType() == SectorType::Background)
		{
			throw BuildingException(&mBuilding,
				"An Agent cannot occupy a Background: it owns no walkable floor and takes no part in traversal");
		}
		auto rawAgent = agent.get();
		rawAgent->attachToBuilding(&mBuilding);
		sector->enterAgent(rawAgent);
		auto id = mBuilding.mAgents.add(std::move(agent));
		mBuilding.mAgentIds.emplace(rawAgent, id);

		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::AgentAdded;
		event.agent = makeAgentSnapshot(rawAgent);
		mBuilding.mEvents.push_back(std::move(event));
		return id;
	}

	AgentId SimulationCoordinator::createAgent(string const& name, uint32_t sectorId, uint32_t deckOffset, float xOffset)
	{
		return addOwnedAgentToSector(make_unique<Agent>(name), sectorId, deckOffset, xOffset);
	}

	AgentId SimulationCoordinator::createAgent(string const& name, uint32_t sectorId)
	{
		return addOwnedAgentToSector(make_unique<Agent>(name), sectorId);
	}

	void SimulationCoordinator::wakeAllAgents()
	{
		for (auto const& [id, agent] : mBuilding.mAgents.entries())
		{
			(void)id;
			// A deactivated Agent is not simulated, so waking the world must not
			// restart its locomotion (#118).
			if (!agent->isActive()) continue;
			agent->wake();
		}
	}

	bool SimulationCoordinator::canSetAgentActive(AgentId id, bool /* active */, string* diagnostic) const
	{
		auto found = lookupAgent(id);
		if (!found)
		{
			if (diagnostic) *diagnostic = found.diagnostic;
			return false;
		}
		if (!mBuilding.mSimulationPaused)
		{
			if (diagnostic)
				*diagnostic = "Agents cannot be activated or deactivated while the simulation is running";
			return false;
		}
		return true;
	}

	bool SimulationCoordinator::setAgentActive(AgentId id, bool active, string* diagnostic)
	{
		if (!canSetAgentActive(id, active, diagnostic)) return false;
		mBuilding.mAgents.find(id)->setActive(active);
		return true;
	}

	EntityLookup<Agent> SimulationCoordinator::lookupAgent(AgentId id)
	{
		auto entity = mBuilding.mAgents.find(id);
		return entity ? EntityLookup<Agent>{ entity, {} }
			: EntityLookup<Agent>{ nullptr, format("Agent handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<Agent const> SimulationCoordinator::lookupAgent(AgentId id) const
	{
		auto entity = mBuilding.mAgents.find(id);
		return entity ? EntityLookup<Agent const>{ entity, {} }
			: EntityLookup<Agent const>{ nullptr, format("Agent handle {} is invalid or has been removed", id.value) };
	}

	AgentId SimulationCoordinator::getAgentId(Agent const* agent) const
	{
		auto found = mBuilding.mAgentIds.find(agent);
		return found == mBuilding.mAgentIds.end() ? AgentId{} : found->second;
	}

	bool SimulationCoordinator::holdsTraversalOwnership(AgentId id) const
	{
		if (!id) return false;

		for (auto const& [requestId, request] : mBuilding.mTraversalRequests.entries())
		{
			(void)requestId;
			if (request->mOwner == id) return true;
		}
		for (auto const& [permitId, permit] : mBuilding.mTraversalPermits.entries())
		{
			(void)permitId;
			if (permit->mOwner == id) return true;
		}
		for (auto const& [resourceId, resource] : mBuilding.mTraversalResources.entries())
		{
			(void)resourceId;
			if (find(resource->mOccupants.begin(), resource->mOccupants.end(), id)
				!= resource->mOccupants.end()) return true;
			if (resource->mExtensionOccupantLeases.contains(id)) return true;
			if (resource->mLiftExitAtSafeStop.contains(id)) return true;
			if (resource->mLiftExitFailures.contains(id)) return true;
			if (resource->mLiftPassengerDestinations.contains(id)) return true;
			if (resource->mLiftTripIntents.contains(id)) return true;
			if (resource->mLiftPassenger == id) return true;
			for (auto const& owners : resource->mLiftStopRequestOwners)
				if (owners.contains(id)) return true;
			for (auto const& ticks : resource->mLiftStopRequestTicks)
				if (ticks.contains(id)) return true;
		}
		return false;
	}

	void SimulationCoordinator::releaseAgentFromResource(TraversalResource& resource, AgentId id)
	{
		if (!id) return;

		for (auto& occupant : resource.mOccupants)
			if (occupant == id) occupant = {};

		// An occupant lease keeps an extensible resource extended on the Agent's
		// behalf; surrendering the slot must surrender the lease with it.
		if (resource.mExtensionOccupantLeases.erase(id) && resource.mExtensible)
			resource.mExtensible->releaseExtensionLease();

		for (uint32_t stop = 0; stop < resource.mLiftStopRequestOwners.size(); ++stop)
			removeLiftStopRequest(resource, stop, id);
		resource.mLiftPassengerDestinations.erase(id);
		resource.mLiftTripIntents.erase(id);
		resource.mLiftExitAtSafeStop.erase(id);
		resource.mLiftExitFailures.erase(id);

		// The compatibility aliases mirror the manifest. Rebuild them from whatever
		// is left rather than leave them naming a handle which can no longer ride.
		if (resource.mLiftPassenger == id)
		{
			resource.mLiftPassenger = {};
			for (auto occupant : resource.mOccupants)
				if (occupant) { resource.mLiftPassenger = occupant; break; }
			resource.mLiftDestinationStop = ~0u;
		}
	}

	void SimulationCoordinator::releaseTraversalOwnership(AgentId id)
	{
		if (!id) return;

		// Requests and permits go first. Most of a resource's claims on an Agent are
		// keyed by request - queue lanes, admission reservations, door open leases,
		// extension request leases - and cancelling the request surrenders them all
		// through the same paths ordinary cancellation uses. No safe transport exit is
		// requested: the Agent is on its way out of the Building entirely.
		std::vector<TraversalRequestId> requests;
		for (auto const& [requestId, request] : mBuilding.mTraversalRequests.entries())
			if (request->mOwner == id) requests.push_back(requestId);
		for (auto requestId : requests)
		{
			auto request = mBuilding.mTraversalRequests.find(requestId);
			if (!request) continue;
			auto const permitId = request->mPermit;
			cancelTraversal(requestId, permitId, false);
			releaseTraversal(requestId, permitId);
		}

		// A permit whose request has already gone is still the Agent's handle.
		std::vector<TraversalPermitId> permits;
		for (auto const& [permitId, permit] : mBuilding.mTraversalPermits.entries())
			if (permit->mOwner == id) permits.push_back(permitId);
		for (auto permitId : permits)
		{
			auto permit = mBuilding.mTraversalPermits.find(permitId);
			if (!permit) continue;
			releaseTraversal(permit->mRequest, permitId);
		}

		// Finally the claims keyed by Agent itself, which survive every request having
		// been released: the manifest slot of a car the Agent boarded, its stop
		// requests, its pending safe exit, and its occupant leases.
		for (auto const& [resourceId, resource] : mBuilding.mTraversalResources.entries())
		{
			(void)resourceId;
			releaseAgentFromResource(*resource, id);
		}
	}

	MovementCommandResult SimulationCoordinator::moveAgentToMarker(AgentId id, MarkerId marker)
	{
		auto agent = mBuilding.mAgents.find(id);
		if (!agent) return { MovementCommandStatus::UnknownAgent };
		if (!agent->isActive()) return { MovementCommandStatus::InactiveAgent };
		if (!mBuilding.lookupMarker(marker)) return { MovementCommandStatus::UnknownMarker };
		if (auto it = mBuilding.mMovementGoals.find(id); it != mBuilding.mMovementGoals.end())
			return { !it->second.cancelling && it->second.marker == marker
				? MovementCommandStatus::NoOp : MovementCommandStatus::AgentBusy };
		if (agent->mPath.path || mBuilding.mPausedPathIntents.contains(id) || holdsTraversalOwnership(id))
			return { MovementCommandStatus::AgentBusy };
		if (!mBuilding.mGraph || mBuilding.mTopologyDirty || !mBuilding.mTopologyValid)
			return { MovementCommandStatus::TopologyUnavailable };
		shared_ptr<const Vertex> target;
		for (auto const& sector : mBuilding.mSectors)
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
					object && object->getMarker()->getId() == marker)
					target = mBuilding.mGraph->getVertexForObject(object);
		auto path = target ? mBuilding.mGraph->calculatePath(agent, target) : nullptr;
		mBuilding.mMovementGoals[id] = { marker, target ? target->getPosition() : Vector2::ZERO, false,
			target ? SectorId{ (uint64_t)target->getSector()->getIndex() + 1 } : SectorId{},
			!path || path->nodes.empty() };
		if (path && !path->nodes.empty()) agent->assignPath(std::move(path), true, false);
		return { MovementCommandStatus::Accepted };
	}

	MovementCommandResult SimulationCoordinator::cancelAgentMovement(AgentId id)
	{
		auto agent = mBuilding.mAgents.find(id);
		if (!agent) return { MovementCommandStatus::UnknownAgent };
		if (!agent->isActive()) return { MovementCommandStatus::InactiveAgent };
		auto it = mBuilding.mMovementGoals.find(id);
		if (it == mBuilding.mMovementGoals.end() && !agent->mPath.path && !holdsTraversalOwnership(id))
			return { MovementCommandStatus::NoOp };
		auto& goal = mBuilding.mMovementGoals[id];
		if (goal.cancelling) return { MovementCommandStatus::NoOp };
		goal.cancelling = true;
		return { MovementCommandStatus::Accepted };
	}

	void SimulationCoordinator::updateMovementGoals()
	{
		for (auto it = mBuilding.mMovementGoals.begin(); it != mBuilding.mMovementGoals.end();)
		{
			auto id = it->first;
			auto const& goal = it->second;
			auto agent = mBuilding.mAgents.find(id);
			if (!agent) { it = mBuilding.mMovementGoals.erase(it); continue; }
			if (!agent->isActive()) { ++it; continue; }
			if (goal.cancelling)
			{
				// Finish an in-flight crossing and any occupied resource journey first.
				// Transport cancellation uses the already scheduled destination stop:
				// do not release a manifest slot or strand a passenger in a Transit.
				bool riding = false;
				for (auto const& [resourceId, resource] : mBuilding.mTraversalResources.entries())
				{
					(void)resourceId;
					if (find(resource->mOccupants.begin(), resource->mOccupants.end(), id) != resource->mOccupants.end()) riding = true;
				}
				bool crossing = agent->mTraversalTask && agent->mTraversalTask->destinationVertex
					&& (agent->mTraversalTask->destinationVertex->getSector().get() != agent->getSector()
						|| agent->mTraversalTask->edge->getTraversalResourceId());
				if (riding || (crossing && (agent->mState == Agent::State::TraversingEdge
					|| agent->mState == Agent::State::AwaitingTraversalCommit)))
				{ ++it; continue; }
				agent->clearRuntimePath();
				releaseTraversalOwnership(id);
				vector<InteractionRequestId> interactions;
				for (auto const& [requestId, request] : mBuilding.mInteractionRequests.entries())
					if (request->getActor() == id && request->getResult() == InteractionResult::Pending) interactions.push_back(requestId);
				for (auto requestId : interactions) cancelInteraction(requestId);
				for (auto const& [operationId, operation] : mBuilding.mDeviceOperations.entries())
					if (operation->getRequesters().contains(id)) cancelDeviceOperation(operationId, id);
			}
			else if (agent->mPath.path) { ++it; continue; }
			SimulationEvent event;
			event.sequence = mBuilding.mNextEventSequence++;
			event.tick = mBuilding.mSimulationTick;
			event.phase = mBuilding.mCurrentPhase;
			event.agent = makeAgentSnapshot(agent);
			event.destinationMarker = goal.marker;
			event.type = goal.cancelling ? SimulationEventType::MovementCancelled
				: !goal.unreachable && mBuilding.lookupMarker(goal.marker) && agent->getSector()
					&& SectorId{ (uint64_t)agent->getSector()->getIndex() + 1 } == goal.sector
					&& agent->getGlobalPosition().distanceTo(goal.position) < 0.001f
					? SimulationEventType::DestinationReached : SimulationEventType::RouteLost;
			if (event.type == SimulationEventType::RouteLost)
				event.routeLossReason = !mBuilding.lookupMarker(goal.marker) ? RouteLossReason::DestinationRemoved
					: goal.unreachable ? RouteLossReason::Unreachable : RouteLossReason::TopologyChanged;
			it = mBuilding.mMovementGoals.erase(it);
			// Runtime observation is a separate subscription: it never drains or
			// mutates the public simulation event queue.
			mBuilding.mAgentBehaviourRuntime->observeOutcome(event);
			mBuilding.mEvents.push_back(std::move(event));
		}
	}

	EntityRemovalResult SimulationCoordinator::removeAgent(AgentId id)
	{
		auto found = lookupAgent(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		if (found.entity->getState() == Agent::State::WaitingForTraversal
			&& !found.entity->getTraversalPermitId())
		{
			// Removing a waiter is cancellation, not an exceptional state. Its
			// queue ticket and physical reservation are released by clearPath().
			found.entity->clearPath();
		}
		if (found.entity->getState() != Agent::State::Idle)
		{
			return { false, format("Agent handle {} is active and cannot be removed safely", id.value) };
		}

		// Idle is not the same as unclaimed. clearPath() asks a Lift or Shuttle to let
		// a rider off when it is next safe rather than ejecting them from a moving
		// car, so the manifest keeps naming the Agent after its route is gone. Every
		// one of those claims is surrendered here; a handle left behind could never
		// disembark, and the capacity would be lost for the life of the Building.
		found.entity->cancelTraversal();
		releaseTraversalOwnership(id);
		if (holdsTraversalOwnership(id))
		{
			return { false, format("Agent handle {} still holds a traversal resource and cannot be removed", id.value) };
		}

		vector<InteractionRequestId> ownedRequests;
		for (auto const& [requestId, request] : mBuilding.mInteractionRequests.entries())
		{
			if (request->getActor() == id && request->getResult() == InteractionResult::Pending)
			{
				ownedRequests.push_back(requestId);
			}
		}
		for (auto requestId : ownedRequests)
		{
			mBuilding.cancelInteraction(requestId);
		}

		vector<DeviceOperationId> ownedOperations;
		for (auto const& [operationId, operation] : mBuilding.mDeviceOperations.entries())
		{
			if (operation->getRequesters().contains(id))
			{
				ownedOperations.push_back(operationId);
			}
		}
		for (auto operationId : ownedOperations)
		{
			mBuilding.cancelDeviceOperation(operationId, id);
			if (auto operation = mBuilding.mDeviceOperations.find(operationId); operation && operation->getRequesters().empty())
			{
				(void)mBuilding.removeDeviceOperation(operationId);
			}
		}

		auto snapshot = makeAgentSnapshot(found.entity);
		if (auto sector = const_cast<Sector*>(found.entity->getSector()))
		{
			sector->exitAgent(found.entity);
		}
		mBuilding.mAgentIds.erase(found.entity);
		mBuilding.mAgents.remove(id);

		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::AgentRemoved;
		event.agent = std::move(snapshot);
		mBuilding.mEvents.push_back(std::move(event));
		mBuilding.markModified();
		return { true, {} };
	}

} // core
