#include <algorithm>
#include <format>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/Building.h"
#include "core/Coordination.h"
#include "core/ExtensibleObject.h"
#include "core/OpenableObject.h"
#include "core/Simulation.h"


namespace core
{

	using namespace std;

	// Snapshot building moved out of Building (ADR 0004 stage 5). Every
	// snapshot - the per-entity builders and the whole-world
	// getSimulationSnapshot - is a read-only projection of Building's
	// registries: agents, interaction points and requests, device operations,
	// traversal resources with their lift, shuttle, queue-lane and crossing-lane
	// detail, traversal requests and traversal permits.
	//
	// The behaviour is unchanged: the bodies are the ones Building held, with
	// the registries and the simulation clock reached through the Building this
	// coordinator was given. Nothing here mutates the world; the coordinator is
	// a friend of the coordination types (ADR 0004) rather than these types
	// gaining a public read surface for it.
	//
	// The other coordinator seams call these builders directly instead of
	// calling back through the Building facade (design pattern, not the Facade
	// sector type) when they fill an event payload; callers outside Building
	// still reach the whole-world snapshot through Building::getSimulationSnapshot.

	AgentSnapshot SimulationCoordinator::makeAgentSnapshot(Agent const* agent) const
	{
		AgentSnapshot result;
		result.id = getAgentId(agent);
		result.name = agent->getName();
		result.sectorId = SectorId{ agent->getSector() ? (uint64_t)agent->getSector()->getIndex() + 1 : 0 };
		result.localPosition = agent->getLocalPosition();
		result.globalPosition = agent->getGlobalPosition();
		result.active = agent->isActive();
		result.hasPath = (bool)agent->getPath();
		result.targetPathNode = agent->getPathTargetNodeIndex();
		result.pathNodeCount = result.hasPath ? (uint32_t)agent->getPath()->nodes.size() : 0;
		result.hasLocomotionTask = agent->hasActiveLocomotionTask();
		result.traversalRequest = agent->getTraversalRequestId();
		result.traversalPermit = agent->getTraversalPermitId();
		for (auto const& [requestId, request] : mBuilding.mInteractionRequests.entries())
		{
			if (request->getActor() == result.id && request->getResult() == InteractionResult::Pending)
			{
				result.interactionRequest = requestId;
				result.hasLocomotionTask = true;
				break;
			}
		}

		switch (agent->getState())
		{
		case Agent::State::Idle:
			result.state = AgentPathState::Idle;
			break;
		case Agent::State::MovingToVertex:
			result.state = AgentPathState::MovingToVertex;
			break;
		case Agent::State::WaitingForTraversal:
			result.state = AgentPathState::WaitingForTraversal;
			break;
		case Agent::State::TraversingEdge:
			result.state = AgentPathState::TraversingEdge;
			break;
		case Agent::State::AwaitingTraversalCommit:
			result.state = AgentPathState::AwaitingTraversalCommit;
			break;
		}

		return result;
	}

	InteractionPointSnapshot SimulationCoordinator::makeInteractionPointSnapshot(InteractionPointId id, InteractionPoint const& point) const
	{
		return { id, point.getName(), point.getSector(), point.getPosition(), point.getReach(),
			point.getDurationTicks(), point.getActiveRequest() };
	}

	InteractionRequestSnapshot SimulationCoordinator::makeInteractionRequestSnapshot(InteractionRequestId id, InteractionRequest const& request) const
	{
		InteractionRequestSnapshot result;
		result.id = id;
		result.point = request.getPoint();
		result.actor = request.getActor();
		result.result = request.getResult();
		for (auto const& [operation, requirement] : request.getOperations())
		{
			(void)requirement;
			result.operations.push_back(operation);
		}
		return result;
	}

	DeviceOperationSnapshot SimulationCoordinator::makeDeviceOperationSnapshot(DeviceOperationId id, DeviceOperation const& operation) const
	{
		DeviceOperationSnapshot result;
		result.id = id;
		result.name = operation.getName();
		result.requester = operation.getRequester();
		result.requesters.assign(operation.getRequesters().begin(), operation.getRequesters().end());
		result.hasCommand = operation.hasCommand();
		if (result.hasCommand)
		{
			result.command = operation.getCommand();
		}
		result.state = operation.getState();
		return result;
	}

	TraversalResourceSnapshot SimulationCoordinator::makeTraversalResourceSnapshot(TraversalResourceId id, TraversalResource const& resource) const
	{
		TraversalResourceSnapshot result;
		result.id = id;
		result.name = resource.getName();
		result.isDoor = resource.mDoor != nullptr;
		result.isWindow = resource.mWindow != nullptr;
		result.windowNormallyTraversable = resource.mWindow && resource.mWindow->isNormallyTraversable();
		result.isLadder = resource.mLadder != nullptr;
		result.isForceBridge = resource.mForceBridge != nullptr;
		result.isLift = resource.mLift != nullptr;
		result.isOpenPlatformLift = resource.mOpenPlatformLift;
		result.virtualBoundaryOwners = resource.mVirtualBoundaryOwners;
		result.virtualBoundaryCrossingCount = (uint32_t)count_if(
			resource.mVirtualBoundaryOwners.begin(), resource.mVirtualBoundaryOwners.end(),
			[](auto owner) { return (bool)owner; });
		result.isShuttle = resource.mShuttle != nullptr;
		result.shuttleCapacityPerCarriage = resource.mShuttleCapacityPerCarriage;
		result.liftMoving = resource.mLiftMoving;
		result.liftCarDoorOpen = resource.mLiftCarDoorOpen;
		result.liftStopPhase = resource.mLiftStopPhase;
		result.liftServiceStartedTick = resource.mLiftServiceStartedTick;
		result.liftBoardingCutoffTick = resource.mLiftBoardingCutoffTick;
		result.liftAcceptingBoarders = (resource.mLift || resource.mShuttle) && resource.mEnabled && !resource.mLiftMoving
			&& resource.mLiftStopPhase == LiftStopPhase::Boarding
			&& mBuilding.mSimulationTick <= resource.mLiftBoardingCutoffTick;
		result.liftDraining = resource.mLiftDraining;
		result.liftPendingSafeExits = (uint32_t)resource.mLiftExitAtSafeStop.size();
		result.liftCurrentStop = resource.mLiftCurrentStop;
		result.liftTargetStop = resource.mLiftTargetStop;
		result.liftDirection = resource.mLiftDirection;
		result.liftPosition = resource.mLiftPosition;
		result.liftSector = resource.mLiftSector;
		result.liftPassenger = resource.mLiftPassenger;
		result.liftAdmissionReservation = resource.mLiftAdmissionReservation;
		result.liftDestinationStop = resource.mLiftDestinationStop;
		result.liftSelector = resource.mLiftSelector;
		result.liftActiveConfirmation = resource.mLiftActiveConfirmation;
		result.liftConfirmationQueue = resource.mLiftConfirmationQueue;
		for (uint32_t stop = 0; stop < resource.mLiftStopRequestOwners.size(); ++stop)
		{
			result.liftStopRequestOwnerCounts.push_back((uint32_t)resource.mLiftStopRequestOwners[stop].size());
			auto const& ticks = resource.mLiftStopRequestTicks[stop];
			auto oldest = ticks.empty() ? ticks.end() : min_element(ticks.begin(), ticks.end(),
				[](auto const& left, auto const& right)
				{ return left.second != right.second ? left.second < right.second : left.first < right.first; });
			result.liftStopOldestRequestTicks.push_back(oldest == ticks.end() ? 0 : oldest->second);
			if (!resource.mLiftStopRequestOwners[stop].empty()) result.liftScheduledStops.push_back(stop);
		}
		if (resource.mLift || resource.mShuttle)
		{
			for (auto const& [agentId, agent] : mBuilding.mAgents.entries())
			{
				auto request = mBuilding.mTraversalRequests.find(agent->getTraversalRequestId());
				bool associatedRequest = false;
				if (request)
				{
					auto requestResource = mBuilding.mTraversalResources.find(request->mResource);
					associatedRequest = request->mResource == id
						|| (requestResource && requestResource->mLiftCoordinator == id);
				}
				bool const occupant = find(resource.mOccupants.begin(), resource.mOccupants.end(), agentId)
					!= resource.mOccupants.end();
				bool const queued = resource.mLiftTripIntents.contains(agentId);
				if (!associatedRequest && !occupant && !queued) continue;

				LiftAgentSnapshot passenger;
				passenger.agent = agentId;
				if (associatedRequest && request->mSourceSector == resource.mLiftSector
					&& request->mDestinationSector != resource.mLiftSector)
					passenger.state = LiftAgentState::Exiting;
				else if (associatedRequest && request->mSourceSector != resource.mLiftSector
					&& request->mDestinationSector == resource.mLiftSector
					&& request->mState != TraversalRequestState::Pending)
					passenger.state = LiftAgentState::Entering;
				else if (occupant) passenger.state = LiftAgentState::InLift;
				else passenger.state = LiftAgentState::QueuingAtDoor;

				auto intent = resource.mLiftTripIntents.find(agentId);
				auto destination = resource.mLiftPassengerDestinations.find(agentId);
				passenger.targetStop = intent != resource.mLiftTripIntents.end()
					? intent->second.destinationStop
					: destination != resource.mLiftPassengerDestinations.end()
						? destination->second : findAgentLiftDestination(*agent, resource);
				if (passenger.targetStop < resource.mLiftStops.size())
					passenger.targetFloor = resource.mLiftStops[passenger.targetStop].globalPosition;
				result.liftAgents.push_back(passenger);
			}
		}
		result.liftAligned = !resource.mLiftMoving && resource.mLiftCurrentStop < resource.mLiftStops.size()
			&& abs(resource.mLiftPosition - resource.mLiftStops[resource.mLiftCurrentStop].globalPosition) < 0.001f;
		result.isExtensible = resource.mExtensible && resource.mExtensible->isExtensible();
		result.extended = resource.mExtensible && resource.mExtensible->isExtended();
		result.retractionPending = resource.mRetractionPending;
		result.extensionRequestLeaseCount = (uint32_t)resource.mExtensionRequestLeases.size();
		result.extensionOccupantLeaseCount = (uint32_t)resource.mExtensionOccupantLeases.size();
		result.isNarrowStairwell = resource.mStairwell != nullptr;
		result.enabled = resource.mEnabled;
		result.capacity = resource.mCapacity;
		result.agentSpacing = resource.mLadderSpacing;
		result.capacitySector = resource.mLadderSector;
		result.admissionQueue = resource.mAdmissionQueue;
		result.activeDirection = resource.mActiveDirection;
		result.directionalBatchCount = resource.mDirectionalBatchCount;
		result.directionalBatchLimit = resource.mDirectionalBatchLimit;
		for (auto requestId : resource.mAdmissionQueue)
		{
			if (auto request = mBuilding.mTraversalRequests.find(requestId))
			{
				if (request->mDirection == TraversalDirection::Ascending) ++result.ascendingWaitingCount;
				else if (request->mDirection == TraversalDirection::Descending) ++result.descendingWaitingCount;
			}
		}
		for (uint32_t i = 0; i < resource.mCapacityPositions.size(); ++i)
		{
			result.capacityPositions.push_back({ i, resource.mCapacityPositions[i],
				resource.mOccupants[i], resource.mAdmissionReservations[i] });
			if (resource.mOccupants[i]) ++result.occupantCount;
			if (resource.mAdmissionReservations[i]) ++result.admissionReservationCount;
		}
		for (auto const& carriage : resource.mShuttleCarriages)
		{
			ShuttleCarriageSnapshot snapshot;
			snapshot.index = carriage.index;
			snapshot.capacity = carriage.capacity;
			snapshot.stopDoors = carriage.stopDoors;
			for (uint32_t i = 0; i < carriage.capacity; ++i)
			{
				auto position = carriage.firstCapacityPosition + i;
				if (position >= resource.mCapacityPositions.size()) break;
				snapshot.positions.push_back({ i, resource.mCapacityPositions[position],
					resource.mOccupants[position], resource.mAdmissionReservations[position] });
				if (resource.mOccupants[position]) ++snapshot.occupantCount;
				if (resource.mAdmissionReservations[position]) ++snapshot.admissionReservationCount;
			}
			result.shuttleCarriages.push_back(std::move(snapshot));
		}
		if (resource.mShuttle)
		{
			for (auto const& door : resource.mShuttleDoors)
				for (auto direction : { TraversalDirection::Ascending, TraversalDirection::Descending })
					if (find_if(result.shuttleAccessZones.begin(), result.shuttleAccessZones.end(),
						[&](auto const& value) { return value.stopIndex == door.stopIndex
							&& value.accessZoneIndex == door.accessZoneIndex
							&& value.direction == direction; }) == result.shuttleAccessZones.end())
						result.shuttleAccessZones.push_back({ door.stopIndex, door.accessZoneIndex,
							door.locationSector, direction, {} });
			for (auto const& [requestId, request] : mBuilding.mTraversalRequests.entries())
			{
				if (!request->mQueueTicket || request->mSourceSector == resource.mLiftSector
					|| request->mState != TraversalRequestState::Pending) continue;
				auto authority = mBuilding.mTraversalResources.find(request->mResource);
				if (!authority || authority->mLiftCoordinator != id) continue;
				auto intent = resource.mLiftTripIntents.find(request->mOwner);
				if (intent == resource.mLiftTripIntents.end()) continue;
				auto direction = resource.mLiftStops[intent->second.destinationStop].globalPosition
					> resource.mLiftStops[intent->second.originStop].globalPosition
					? TraversalDirection::Ascending : TraversalDirection::Descending;
				auto zone = request->mShuttleAccessZone;
				if (zone == ~0u)
				{
					auto door = find_if(resource.mShuttleDoors.begin(), resource.mShuttleDoors.end(),
						[&](auto const& value) { return value.landingResource == request->mResource; });
					if (door != resource.mShuttleDoors.end()) zone = door->accessZoneIndex;
				}
				auto found = find_if(result.shuttleAccessZones.begin(), result.shuttleAccessZones.end(),
					[&](auto const& value) { return value.stopIndex == intent->second.originStop
						&& value.accessZoneIndex == zone && value.direction == direction; });
				if (found == result.shuttleAccessZones.end())
				{
					result.shuttleAccessZones.push_back({ intent->second.originStop, zone,
						request->mSourceSector, direction, { requestId } });
				}
				else found->queue.push_back(requestId);
			}
			for (auto& zone : result.shuttleAccessZones)
				sort(zone.queue.begin(), zone.queue.end(), [&](auto left, auto right)
				{
					auto lhs = mBuilding.mTraversalRequests.find(left);
					auto rhs = mBuilding.mTraversalRequests.find(right);
					return lhs && rhs ? lhs->mQueueTicket < rhs->mQueueTicket : left < right;
				});
		}
		result.doorActivationMode = resource.mDoorActivationMode;
		result.holdOpenTicks = resource.mHoldOpenTicks;
		result.openLeaseCount = (uint32_t)resource.mOpenLeases.size();
		for (auto const& [leaseId, lease] : resource.mOpenLeases)
		{
			(void)leaseId;
			switch (lease.kind)
			{
			case DoorOpenLeaseKind::Preparation: ++result.preparationLeaseCount; break;
			case DoorOpenLeaseKind::Crossing: ++result.crossingLeaseCount; break;
			case DoorOpenLeaseKind::ExternalHoldOpen: ++result.externalOpenLeaseCount; break;
			}
		}
		for (auto const& [sensor, observation] : resource.mSensorObservations)
		{
			(void)sensor;
			result.presenceObserved = result.presenceObserved || observation == DoorSensorObservation::Presence;
			result.obstructionObserved = result.obstructionObserved || observation == DoorSensorObservation::Obstruction;
		}
		result.controls = resource.mControls;
		result.activePreparation = resource.mActivePreparation;
		result.preparationOperator = resource.mPreparationOperator;
		for (uint32_t i = 0; i < resource.mCrossingOwners.size(); ++i)
		{
			result.crossingLanes.push_back({ i, resource.mCrossingOwners[i] });
			if (!result.crossingOwner && resource.mCrossingOwners[i])
			{
				result.crossingOwner = resource.mCrossingOwners[i];
			}
		}
		for (auto const& lane : resource.mQueueLanes)
		{
			if (!lane.sector)
			{
				continue;
			}
			QueueLaneSnapshot queueLaneSnapshot;
			queueLaneSnapshot.sector = lane.sector;
			queueLaneSnapshot.origin = lane.origin;
			queueLaneSnapshot.direction = lane.direction;
			queueLaneSnapshot.extent = lane.extent;
			queueLaneSnapshot.queue = lane.queue;
			for (uint32_t i = 0; i < lane.positions.size(); ++i)
			{
				queueLaneSnapshot.positions.push_back({ i, lane.positions[i], lane.positionOwners[i] });
			}
			result.queueLanes.push_back(std::move(queueLaneSnapshot));
		}
		if (resource.mDoor)
		{
			result.doorOpenPercentage = resource.mDoor->getOpenPercentage();
			switch (resource.mDoor->getState())
			{
			case OpenableObject::State::Closed: result.doorState = DoorSnapshotState::Closed; break;
			case OpenableObject::State::Opening: result.doorState = DoorSnapshotState::Opening; break;
			case OpenableObject::State::Open: result.doorState = DoorSnapshotState::Open; break;
			case OpenableObject::State::Closing: result.doorState = DoorSnapshotState::Closing; break;
			}
		}
		return result;
	}

	TraversalRequestSnapshot SimulationCoordinator::makeTraversalRequestSnapshot(TraversalRequestId id, TraversalRequest const& request) const
	{
		TraversalRequestSnapshot result{ id, request.getOwner(), request.getEdgeType(), request.getSourceSector(),
			request.getDestinationSector(), request.getSourceEndpoint(), request.getDestinationEndpoint(),
			request.getState(), request.getResource(), request.getPreparationOperation(), request.getPermit(),
			request.getFailureReason() };
		result.queueTicket = request.mQueueTicket;
		result.queuedAtTick = request.mQueuedAtTick;
		result.queueApproach = request.mQueueApproach;
		result.hasQueuePosition = request.mQueuePosition != ~0u;
		result.queuePosition = request.mQueuePosition;
		result.hasCrossingLane = request.mCrossingLane != ~0u;
		result.crossingLane = request.mCrossingLane;
		result.hasCapacityPosition = request.mCapacityPosition != ~0u;
		result.capacityPosition = request.mCapacityPosition;
		result.shuttleCarriage = request.mShuttleCarriage;
		result.shuttleAccessZone = request.mShuttleAccessZone;
		result.shuttleDoor = request.mShuttleDoor;
		result.direction = request.mDirection;
		result.positionAssignedAtTick = request.mPositionAssignedAtTick;
		result.lastPositionProgressTick = request.mLastPositionProgressTick;
		result.positionRetryAtTick = request.mPositionRetryAtTick;
		result.positionRetryCount = request.mPositionRetryCount;

		auto failureDiagnostic = [](TraversalFailureReason reason)
		{
			switch (reason)
			{
			case TraversalFailureReason::NoReachableControl: return "no reachable interaction point can prepare the resource";
			case TraversalFailureReason::ControlRejected: return "the resource control rejected the request";
			case TraversalFailureReason::PreparationFailed: return "resource preparation failed";
			case TraversalFailureReason::ResourceDisabled: return "the traversal resource is disabled";
			case TraversalFailureReason::LocalGoalUnreachable: return "the assigned local waiting position is unreachable";
			case TraversalFailureReason::PermitExpired: return "the traversal permit expired before progress was made";
			case TraversalFailureReason::None: return "no failure was reported";
			}
			return "unknown traversal failure";
		};
		switch (request.mState)
		{
		case TraversalRequestState::Denied:
			result.diagnostic = string("Denied: ") + failureDiagnostic(request.mFailureReason);
			break;
		case TraversalRequestState::Cancelled:
			result.diagnostic = "Cancelled: all traversal ownership is being released";
			break;
		case TraversalRequestState::Committed:
			result.diagnostic = "Committed: the authorized sector transition completed";
			break;
		case TraversalRequestState::Granted:
			result.diagnostic = request.mPermit
				? format("Active: permit {} authorizes this transition", request.mPermit.value)
				: "Active: admission was granted and a permit is pending publication";
			break;
		case TraversalRequestState::Pending:
		{
			auto resource = mBuilding.mTraversalResources.find(request.mResource);
			if (!request.mResource)
				result.diagnostic = "Waiting: immediate traversal allocation is pending";
			else if (!resource)
				result.diagnostic = "Waiting: the referenced traversal resource is unavailable";
			else if (!resource->mEnabled)
				result.diagnostic = "Waiting: the traversal resource is draining or disabled";
			else if (request.mPreparationOperation)
				result.diagnostic = format("Waiting: device operation {} is preparing the resource",
					request.mPreparationOperation.value);
			else if (request.mQueuePosition != ~0u)
				result.diagnostic = format("Waiting: moving to reserved queue position {}",
					request.mQueuePosition);
			else if (request.mQueueTicket)
				result.diagnostic = format("Waiting: queue ticket {} is awaiting a position or admission",
					request.mQueueTicket.value);
			else if (request.mCapacityPosition != ~0u)
				result.diagnostic = format("Waiting: capacity position {} is reserved for boarding",
					request.mCapacityPosition);
			else if ((resource->mLift || resource->mShuttle) && resource->mLiftMoving)
				result.diagnostic = format("Waiting: transport is moving toward stop {}", resource->mLiftTargetStop);
			else
				result.diagnostic = "Waiting: resource admission conditions are not yet satisfied";
			break;
		}
		}
		if (result.hasQueuePosition)
		{
			if (auto resource = mBuilding.mTraversalResources.find(request.mResource);
				resource && request.mQueueApproach < resource->mQueueLanes.size()
				&& request.mQueuePosition < resource->mQueueLanes[request.mQueueApproach].positions.size())
			{
				result.queuePositionTarget = resource->mQueueLanes[request.mQueueApproach].positions[request.mQueuePosition];
			}
		}
		return result;
	}

	TraversalPermitSnapshot SimulationCoordinator::makeTraversalPermitSnapshot(TraversalPermitId id, TraversalPermit const& permit) const
	{
		return { id, permit.getRequest(), permit.getOwner(), permit.getState(), permit.getExpiresAtTick() };
	}

	SimulationSnapshot SimulationCoordinator::getSimulationSnapshot() const
	{
		SimulationSnapshot result;
		result.tick = mBuilding.mSimulationTick;
		result.paused = mBuilding.mSimulationPaused;
		result.topologyDirty = mBuilding.mTopologyDirty;
		result.topologyValid = mBuilding.mTopologyValid;
		result.topologyGeneration = mBuilding.mTopologyGeneration;
		result.topologyDiagnostic = mBuilding.mTopologyDiagnostic;
		result.agents.reserve(mBuilding.mAgents.entries().size());
		result.interactionPoints.reserve(mBuilding.mInteractionPoints.entries().size());
		result.interactionRequests.reserve(mBuilding.mInteractionRequests.entries().size());
		result.deviceOperations.reserve(mBuilding.mDeviceOperations.entries().size());
		result.traversalResources.reserve(mBuilding.mTraversalResources.entries().size());
		result.traversalRequests.reserve(mBuilding.mTraversalRequests.entries().size());
		result.traversalPermits.reserve(mBuilding.mTraversalPermits.entries().size());

		for (auto const& [id, agent] : mBuilding.mAgents.entries())
		{
			(void)id;
			result.agents.push_back(makeAgentSnapshot(agent.get()));
		}
		for (auto const& [id, point] : mBuilding.mInteractionPoints.entries())
		{
			result.interactionPoints.push_back(makeInteractionPointSnapshot(id, *point));
		}
		for (auto const& [id, request] : mBuilding.mInteractionRequests.entries())
		{
			result.interactionRequests.push_back(makeInteractionRequestSnapshot(id, *request));
		}
		for (auto const& [id, operation] : mBuilding.mDeviceOperations.entries())
		{
			result.deviceOperations.push_back(makeDeviceOperationSnapshot(id, *operation));
		}
		for (auto const& [id, resource] : mBuilding.mTraversalResources.entries())
		{
			result.traversalResources.push_back(makeTraversalResourceSnapshot(id, *resource));
		}
		for (auto const& [id, request] : mBuilding.mTraversalRequests.entries())
		{
			result.traversalRequests.push_back(makeTraversalRequestSnapshot(id, *request));
		}
		for (auto const& [id, permit] : mBuilding.mTraversalPermits.entries())
		{
			result.traversalPermits.push_back(makeTraversalPermitSnapshot(id, *permit));
		}

		return result;
	}

} // core
