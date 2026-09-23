#include <algorithm>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/World.h"
#include "core/Coordination.h"
#include "core/Edge.h"
#include "core/ExtensibleObject.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/Vertex.h"


namespace core
{

	using namespace std;

	// The traversal transaction lifecycle moved out of World (ADR 0004
	// stage 4): granting a permit, allocating a pending request across the
	// resource families, denying, committing, cancelling, and releasing a
	// traversal and its permit.
	//
	// The behaviour is unchanged. The coordinator works on World's
	// traversal-request, traversal-permit, interaction and device-operation
	// registries through friendship, calls the queue, admission, lease, lift and
	// snapshot machinery now living beside it directly, and keeps no callback to
	// the World facade (design pattern, not the Facade sector type) in this
	// seam.

	TraversalPermitId SimulationCoordinator::grantTraversalRequest(TraversalRequestId requestId)
	{
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending)
		{
			return {};
		}

		auto permitId = mWorld.mTraversalPermits.add(unique_ptr<TraversalPermit>(
			new TraversalPermit(requestId, request->mOwner)));
		auto permit = mWorld.mTraversalPermits.find(permitId);
		permit->mExpiresAtTick = mWorld.mSimulationTick + mWorld.mTraversalWaitingPolicy.permitProgressTimeoutTicks;
		if (auto agent = mWorld.mAgents.find(request->mOwner))
		{
			permit->mBestDestinationDistance = agent->getGlobalPosition().distanceTo(request->mDestinationEndpoint);
		}
		request->mPermit = permitId;
		request->mState = TraversalRequestState::Granted;
		request->mFailureReason = TraversalFailureReason::None;
		if (auto resource = mWorld.mTraversalResources.find(request->mResource); resource && resource->mDoor)
		{
			request->mCrossingLease = acquireDoorOpenLease(*resource, DoorOpenLeaseKind::Crossing, requestId);
			if (request->mPreparationLease)
			{
				releaseDoorOpenLease(*resource, request->mPreparationLease);
				request->mPreparationLease = {};
			}
		}

		SimulationEvent requestEvent;
		requestEvent.sequence = mWorld.mNextEventSequence++;
		requestEvent.tick = mWorld.mSimulationTick;
		requestEvent.type = SimulationEventType::TraversalRequestChanged;
		requestEvent.phase = mWorld.mCurrentPhase;
		requestEvent.traversalRequest = makeTraversalRequestSnapshot(requestId, *request);
		mWorld.mEvents.push_back(std::move(requestEvent));

		SimulationEvent permitEvent;
		permitEvent.sequence = mWorld.mNextEventSequence++;
		permitEvent.tick = mWorld.mSimulationTick;
		permitEvent.type = SimulationEventType::TraversalPermitAdded;
		permitEvent.phase = mWorld.mCurrentPhase;
		permitEvent.traversalPermit = makeTraversalPermitSnapshot(permitId, *mWorld.mTraversalPermits.find(permitId));
		mWorld.mEvents.push_back(std::move(permitEvent));
		return permitId;
	}

	void SimulationCoordinator::allocateTraversalRequest(TraversalRequestId requestId,
		shared_ptr<const Edge> const& edge, shared_ptr<const Vertex> const& destination)
	{
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending || !edge || !destination)
		{
			return;
		}

		if (request->mResource)
		{
			auto resource = mWorld.mTraversalResources.find(request->mResource);
			if (!resource)
			{
				denyTraversalRequest(requestId);
				return;
			}
			if (resource->mLift || resource->mShuttle || resource->mLiftCoordinator)
			{
				allocateLiftTraversal(requestId, *resource);
				return;
			}
			if (resource->mExtensible && !resource->mExtensible->isExtended())
			{
				allocateExtensiblePreparation(requestId, *resource);
				return;
			}
			if (resource->mForceBridge)
			{
				if (!resource->mEnabled)
				{
					denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
					return;
				}
				if (resource->mPreparationOperator)
				{
					resource->mActivePreparation = {};
					resource->mPreparationOperator = {};
					resource->mSharedPreparationOperation = {};
					refreshQueuePositions(*resource);
				}
				tryGrantDoorQueue(*resource);
				return;
			}
			if (resource->mLadder || resource->mStairwell)
			{
				if (resource->mLadder && resource->mExtensible
					&& resource->mExtensible->isExtended() && resource->mPreparationOperator)
				{
					resource->mActivePreparation = {};
					resource->mPreparationOperator = {};
					resource->mSharedPreparationOperation = {};
					refreshQueuePositions(*resource);
				}
				if (isLadderAdmission(*request, *resource))
				{
					if (!resource->mEnabled)
					{
						denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
						return;
					}
					attachLadderAdmissionRequest(requestId, *resource);
					tryGrantLadderAdmissions(*resource);
				}
				else
				{
					grantTraversalRequest(requestId);
				}
				return;
			}
			if (resource->mWindow)
			{
				if (!resource->mEnabled)
					denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
				else if (resource->mWindow->isNormallyTraversable())
					grantTraversalRequest(requestId);
				else
					denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
				return;
			}
			if (!resource->mDoor)
			{
				denyTraversalRequest(requestId);
				return;
			}
			if (!resource->mEnabled)
			{
				return; // deactivation cleanup denies waiters after active lanes drain
			}
			if (resource->mDoorActivationMode == DoorActivationMode::Unavailable)
			{
				denyTraversalRequest(requestId);
				return;
			}
			if (!request->mPreparationLease)
			{
				request->mPreparationLease = acquireDoorOpenLease(*resource,
					DoorOpenLeaseKind::Preparation, requestId);
			}
			if (resource->mDoorActivationMode == DoorActivationMode::RemoteControlled)
			{
				allocateRemoteDoorPreparation(requestId, *resource);
				return;
			}
			if (resource->mDoor->isOpen())
			{
				tryGrantDoorQueue(*resource);
				return;
			}
			if (!request->mPreparationRequested)
			{
				request->mPreparationRequested = true;
				DeviceCommand command;
				command.type = DeviceCommandType::OpenDoor;
				command.desiredState = true;
				command.traversalResource = request->mResource;
				request->mPreparationOperation = findOrCreateDeviceOperation(command, request->mOwner);
				if (auto operation = mWorld.mDeviceOperations.find(request->mPreparationOperation))
				{
					// Presence activates an automatic door; reaching the threshold and
					// requesting traversal is the manual interaction for a manual door.
					operation->mActivated = true;
				}
			}
			if (auto operation = mWorld.mDeviceOperations.find(request->mPreparationOperation);
				!operation || operation->mState == DeviceOperationState::Failed
				|| operation->mState == DeviceOperationState::Rejected
				|| operation->mState == DeviceOperationState::Cancelled)
			{
				denyTraversalRequest(requestId, operation && operation->mState == DeviceOperationState::Rejected
					? TraversalFailureReason::ControlRejected : TraversalFailureReason::PreparationFailed);
			}
			return;
		}

		shared_ptr<const Agent> agentView(mWorld.mAgents.find(request->mOwner), [](Agent const*) {});
		if (edge->isTraversable(destination, agentView))
		{
			grantTraversalRequest(requestId);
			return;
		}
		if (!request->mPreparationRequested)
		{
			request->mPreparationRequested = true;
			auto result = edge->requestTraversal(destination, agentView);
			if (result == EdgeTraversalRequestResult::Failed)
			{
				denyTraversalRequest(requestId);
			}
			else if (edge->isTraversable(destination, agentView))
			{
				grantTraversalRequest(requestId);
			}
		}
	}

	void SimulationCoordinator::denyTraversalRequest(TraversalRequestId requestId, TraversalFailureReason reason)
	{
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending)
		{
			return;
		}
		request->mState = TraversalRequestState::Denied;
		request->mFailureReason = reason;
		if (auto resource = mWorld.mTraversalResources.find(request->mResource); resource)
		{
			if (resource->mExtensible && resource->mExtensionRequestLeases.erase(requestId))
				resource->mExtensible->releaseExtensionLease();
			if (resource->mDoor || resource->mForceBridge)
			{
				if (resource->mDoor && request->mPreparationLease)
					releaseDoorOpenLease(*resource, request->mPreparationLease);
				request->mPreparationLease = {};
				releaseDoorQueueOwnership(requestId, *resource);
				if (auto lift = mWorld.mTraversalResources.find(resource->mLiftCoordinator))
					releaseLiftAdmission(requestId, *lift);
			}
			else if (resource->mLift || resource->mShuttle)
			{
				releaseLiftAdmission(requestId, *resource);
			}
			else if (resource->mLadder || resource->mStairwell)
			{
				releaseLadderAdmission(requestId, *resource);
				tryGrantLadderAdmissions(*resource);
			}
		}

		SimulationEvent event;
		event.sequence = mWorld.mNextEventSequence++;
		event.tick = mWorld.mSimulationTick;
		event.type = SimulationEventType::TraversalRequestChanged;
		event.phase = mWorld.mCurrentPhase;
		event.traversalRequest = makeTraversalRequestSnapshot(requestId, *request);
		mWorld.mEvents.push_back(std::move(event));
	}

	bool SimulationCoordinator::commitTraversal(Agent& agent, TraversalRequestId requestId, TraversalPermitId permitId,
		shared_ptr<const Vertex> const& destination)
	{
		auto request = mWorld.mTraversalRequests.find(requestId);
		auto permit = mWorld.mTraversalPermits.find(permitId);
		auto owner = getAgentId(&agent);
		auto const commitsAtQueueBoundary = request && agent.mQueuedTraversalTask
			&& request->mEdgeType == EdgeType::Location
			&& request->mSourceSector == request->mDestinationSector;
		auto const commitsAfterSkippedVertex = request && destination && agent.mTraversalTask
			&& agent.mTraversalTask->request == requestId
			&& agent.mTraversalTask->pathNodesConsumed > 1
			&& agent.getGlobalPosition().distanceTo(destination->getPosition()) <= 0.001f;
		// A Door crossing is executed in place at the Agent's current position, so
		// the layer change does not depend on the far-side Door vertex being a
		// movement target the Agent had to reach.
		auto const commitsInPlace = request && request->mEdgeType == EdgeType::Door;
		if (!request || !permit || !destination || request->mOwner != owner || permit->mOwner != owner
			|| permit->mRequest != requestId || request->mPermit != permitId
			|| request->mState != TraversalRequestState::Granted
			|| permit->mState != TraversalPermitState::Active
			|| (!commitsAtQueueBoundary && !commitsAfterSkippedVertex && !commitsInPlace
				&& agent.getGlobalPosition().distanceTo(request->mDestinationEndpoint) > 0.001f))
		{
			return false;
		}

		auto sourceSector = const_cast<Sector*>(agent.getSector());
		auto destinationSector = destination->getSector();
		if (!sourceSector
			|| request->mSourceSector != SectorId{ (uint64_t)sourceSector->getIndex() + 1 }
			|| (!commitsAfterSkippedVertex
				&& request->mDestinationSector != SectorId{ (uint64_t)destinationSector->getIndex() + 1 }))
		{
			return false;
		}

		auto ladderResource = mWorld.mTraversalResources.find(request->mResource);
		if (ladderResource && ladderResource->mOpenPlatformLift && request->mEdgeType == EdgeType::Lift
			&& find(ladderResource->mOccupants.begin(), ladderResource->mOccupants.end(), owner)
				== ladderResource->mOccupants.end()) return false;
		if (auto landing = mWorld.mTraversalResources.find(request->mResource);
			landing && landing->mLiftCoordinator && request->mDestinationSector != request->mSourceSector)
		{
			auto lift = mWorld.mTraversalResources.find(landing->mLiftCoordinator);
			if (!lift) return false;
			if (request->mDestinationSector == lift->mLiftSector
				&& (request->mCapacityPosition >= lift->mCapacity
					|| lift->mAdmissionReservations[request->mCapacityPosition] != requestId
					|| lift->mOccupants[request->mCapacityPosition])) return false;
		}
		if (ladderResource && (ladderResource->mLadder || ladderResource->mStairwell)
			&& isLadderAdmission(*request, *ladderResource))
		{
			if (request->mCapacityPosition >= ladderResource->mCapacity
				|| ladderResource->mAdmissionReservations[request->mCapacityPosition] != requestId
				|| ladderResource->mOccupants[request->mCapacityPosition])
			{
				return false;
			}
		}

		// The transfer is deliberately confined to the commit phase. Until this
		// point movement changed only the source-relative position.
		if (sourceSector != destinationSector.get())
		{
			auto const inPlaceGlobal = agent.getGlobalPosition();
			sourceSector->exitAgent(&agent);
			if (commitsInPlace)
			{
				// Commit the layer change in place: the Agent keeps the global
				// position it crossed from, expressed in the destination Sector's
				// local frame, rather than snapping to the far-side vertex.
				auto const local = inPlaceGlobal - destinationSector->getPosition();
				destinationSector->enterAgent(&agent, SectorPosition(destinationSector.get(), local), false);
			}
			else
			{
				destinationSector->enterAgent(&agent,
					SectorPosition(destinationSector.get(), destination->getSectorOffset()), false);
			}
		}

		if (auto landing = mWorld.mTraversalResources.find(request->mResource);
			landing && landing->mLiftCoordinator)
		{
			auto lift = mWorld.mTraversalResources.find(landing->mLiftCoordinator);
			if (!lift) return false;
			if (request->mSourceSector != lift->mLiftSector
				&& request->mDestinationSector == lift->mLiftSector)
			{
				auto position = request->mCapacityPosition;
				if (position >= lift->mCapacity || lift->mAdmissionReservations[position] != requestId
					|| lift->mOccupants[position]) return false;
				lift->mAdmissionReservations[position] = {};
				lift->mOccupants[position] = owner;
				lift->mLiftAdmissionReservation = {};
				lift->mLiftPassenger = lift->mOccupants.front();
				request->mCapacityPosition = ~0u;
				auto local = lift->mCapacityPositions[position];
				if (lift->mShuttle)
				{
					// Crossing commits occupancy at the carriage threshold. Walking to
					// the reserved interior spot remains ordinary Agent locomotion.
					auto target = destinationSector->getPosition() + local;
					target.x += lift->mLiftPosition - destinationSector->getPosition().x;
					agent.mTraversalLocalGoal = target;
				}
				else
				{
					local.y += lift->mLiftPosition - destinationSector->getPosition().y;
					agent.setPosition({ destinationSector.get(), local }, false);
				}
			}
			else if (request->mSourceSector == lift->mLiftSector
				&& request->mDestinationSector != lift->mLiftSector)
			{
				for (auto& occupant : lift->mOccupants) if (occupant == owner) occupant = {};
				auto destinationIt = lift->mLiftPassengerDestinations.find(owner);
				if (destinationIt != lift->mLiftPassengerDestinations.end())
				{
					removeLiftStopRequest(*lift, destinationIt->second, owner);
					lift->mLiftPassengerDestinations.erase(destinationIt);
				}
				for (uint32_t stop = 0; stop < lift->mLiftStopRequestOwners.size(); ++stop)
					removeLiftStopRequest(*lift, stop, owner);
				lift->mLiftExitAtSafeStop.erase(owner);
				lift->mLiftExitFailures.erase(owner);
				lift->mLiftPassenger = {};
				for (auto occupant : lift->mOccupants) if (occupant) { lift->mLiftPassenger = occupant; break; }
				lift->mLiftDestinationStop = ~0u;
			}
		}

		if (auto resource = ladderResource; resource && resource->mOpenPlatformLift
			&& request->mEdgeType == EdgeType::Lift)
		{
			for (auto& occupant : resource->mOccupants) if (occupant == owner) occupant = {};
			for (uint32_t stop = 0; stop < resource->mLiftStopRequestOwners.size(); ++stop)
				removeLiftStopRequest(*resource, stop, owner);
			resource->mLiftPassengerDestinations.erase(owner);
			resource->mLiftTripIntents.erase(owner);
			resource->mLiftExitAtSafeStop.erase(owner);
			resource->mLiftExitFailures.erase(owner);
			resource->mLiftPassenger = {};
			for (auto passenger : resource->mOccupants) if (passenger) { resource->mLiftPassenger = passenger; break; }
			resource->mLiftDestinationStop = ~0u;
			for (auto& boundaryOwner : resource->mVirtualBoundaryOwners)
				if (boundaryOwner == requestId) boundaryOwner = {};
		}

		if (auto resource = ladderResource; resource && (resource->mLadder || resource->mStairwell))
		{
			// Committing entry converts the provisional slot reservation into occupancy;
			// committing exit frees occupancy. Room Ladders instead release their slot
			// after the in-sector vertical edge has completed.
			if (resource->mLadder && request->mSourceSector != resource->mLadderSector
				&& request->mDestinationSector == resource->mLadderSector
				&& request->mCapacityPosition < resource->mCapacity)
			{
				auto position = request->mCapacityPosition;
				resource->mAdmissionReservations[position] = {};
				resource->mOccupants[position] = owner;
				if (resource->mExtensible)
				{
					if (resource->mExtensionRequestLeases.erase(requestId))
						resource->mExtensible->releaseExtensionLease();
					if (resource->mExtensionOccupantLeases.insert(owner).second)
						resource->mExtensible->acquireExtensionLease();
				}
				request->mCapacityPosition = ~0u;
			}
			else if (resource->mLadder && request->mSourceSector == resource->mLadderSector
				&& request->mDestinationSector != resource->mLadderSector)
			{
				releaseLadderOccupancy(owner, *resource);
				if (resource->mExtensible && resource->mExtensionOccupantLeases.erase(owner))
					resource->mExtensible->releaseExtensionLease();
			}
			else if (request->mCapacityPosition != ~0u)
			{
				// A ladder embedded in one location owns capacity only while its
				// vertical edge is active; there is no separate transit membership.
				releaseLadderAdmission(requestId, *resource);
			}
			// Every capacity-changing commit is an opportunity to admit another waiter.
			tryGrantLadderAdmissions(*resource);
		}

		if (auto resource = mWorld.mTraversalResources.find(request->mResource);
			resource && resource->mForceBridge
			&& resource->mExtensionRequestLeases.erase(requestId))
			resource->mExtensible->releaseExtensionLease();
		permit->mState = TraversalPermitState::Committed;
		request->mState = TraversalRequestState::Committed;

		SimulationEvent permitEvent;
		permitEvent.sequence = mWorld.mNextEventSequence++;
		permitEvent.tick = mWorld.mSimulationTick;
		permitEvent.type = SimulationEventType::TraversalPermitChanged;
		permitEvent.phase = mWorld.mCurrentPhase;
		permitEvent.traversalPermit = makeTraversalPermitSnapshot(permitId, *permit);
		mWorld.mEvents.push_back(std::move(permitEvent));

		SimulationEvent requestEvent;
		requestEvent.sequence = mWorld.mNextEventSequence++;
		requestEvent.tick = mWorld.mSimulationTick;
		requestEvent.type = SimulationEventType::TraversalRequestChanged;
		requestEvent.phase = mWorld.mCurrentPhase;
		requestEvent.traversalRequest = makeTraversalRequestSnapshot(requestId, *request);
		mWorld.mEvents.push_back(std::move(requestEvent));
		return true;
	}

	void SimulationCoordinator::cancelTraversal(TraversalRequestId requestId, TraversalPermitId permitId,
		bool requestSafeTransportExit)
	{
		if (auto request = mWorld.mTraversalRequests.find(requestId))
		{
			// Ordinary route cancellation is not permission to leave a moving car.
			// A topology rebuild is different: the manifest survives and will be
			// rebound to the replacement graph, so it must not invent an exit demand.
			if (requestSafeTransportExit)
				requestLiftPassengerSafeExit(request->mOwner, TraversalFailureReason::None);
			if (auto resource = mWorld.mTraversalResources.find(request->mResource);
				resource && resource->mExtensible && resource->mExtensionRequestLeases.erase(requestId))
				resource->mExtensible->releaseExtensionLease();
			if (auto resource = mWorld.mTraversalResources.find(request->mResource);
				resource && resource->mPreparationOperator == requestId)
			{
				if (resource->mActivePreparation)
				{
					cancelInteraction(resource->mActivePreparation);
				}
				resource->mActivePreparation = {};
				resource->mPreparationOperator = {};
				resource->mSharedPreparationOperation = {};
				resource->mNextPreparationTick = mWorld.mSimulationTick + 1;
			}
			if (auto resource = mWorld.mTraversalResources.find(request->mResource); resource)
			{
				if (resource->mDoor || resource->mForceBridge)
					releaseDoorQueueOwnership(requestId, *resource);
				if (auto lift = mWorld.mTraversalResources.find(resource->mLiftCoordinator))
					releaseLiftAdmission(requestId, *lift);
				else if (resource->mOpenPlatformLift)
				{
					for (auto& boundaryOwner : resource->mVirtualBoundaryOwners)
						if (boundaryOwner == requestId) boundaryOwner = {};
					if (find(resource->mOccupants.begin(), resource->mOccupants.end(), request->mOwner)
						== resource->mOccupants.end()) releaseLiftAdmission(requestId, *resource);
				}
				else if (resource->mLift || resource->mShuttle)
					releaseLiftAdmission(requestId, *resource);
				else if (resource->mLadder || resource->mStairwell)
				{
					releaseLadderAdmission(requestId, *resource);
					tryGrantLadderAdmissions(*resource);
				}
			}
			vector<InteractionRequestId> ownedInteractions;
			for (auto const& [interactionId, interaction] : mWorld.mInteractionRequests.entries())
				if (interaction->mActor == request->mOwner
					&& interaction->mResult == InteractionResult::Pending)
					ownedInteractions.push_back(interactionId);
			for (auto interactionId : ownedInteractions) cancelInteraction(interactionId);
		}

		if (auto permit = mWorld.mTraversalPermits.find(permitId);
			permit && permit->mState == TraversalPermitState::Active)
		{
			permit->mState = TraversalPermitState::Cancelled;
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::TraversalPermitChanged;
			event.phase = mWorld.mCurrentPhase;
			event.traversalPermit = makeTraversalPermitSnapshot(permitId, *permit);
			mWorld.mEvents.push_back(std::move(event));
		}

		if (auto request = mWorld.mTraversalRequests.find(requestId);
			request && request->mState != TraversalRequestState::Committed)
		{
			request->mState = TraversalRequestState::Cancelled;
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::TraversalRequestChanged;
			event.phase = mWorld.mCurrentPhase;
			event.traversalRequest = makeTraversalRequestSnapshot(requestId, *request);
			mWorld.mEvents.push_back(std::move(event));
		}
	}

	void SimulationCoordinator::releaseTraversal(TraversalRequestId requestId, TraversalPermitId permitId)
	{
		if (auto request = mWorld.mTraversalRequests.find(requestId))
		{
			if (auto resource = mWorld.mTraversalResources.find(request->mResource); resource)
			{
				if (resource->mExtensible && resource->mExtensionRequestLeases.erase(requestId))
					resource->mExtensible->releaseExtensionLease();
				if (resource->mDoor || resource->mForceBridge)
				{
					if (resource->mDoor && request->mPreparationLease)
						releaseDoorOpenLease(*resource, request->mPreparationLease);
					if (resource->mDoor && request->mCrossingLease)
						releaseDoorOpenLease(*resource, request->mCrossingLease);
					request->mPreparationLease = {};
					request->mCrossingLease = {};
					releaseDoorQueueOwnership(requestId, *resource);
					if (auto lift = mWorld.mTraversalResources.find(resource->mLiftCoordinator))
						releaseLiftAdmission(requestId, *lift);
				}
				else if (resource->mOpenPlatformLift)
				{
					for (auto& boundaryOwner : resource->mVirtualBoundaryOwners)
						if (boundaryOwner == requestId) boundaryOwner = {};
					releaseLiftAdmission(requestId, *resource);
				}
				else if (resource->mLift || resource->mShuttle)
					releaseLiftAdmission(requestId, *resource);
				else if (resource->mLadder || resource->mStairwell)
				{
					releaseLadderAdmission(requestId, *resource);
					tryGrantLadderAdmissions(*resource);
				}
			}
			if (request->mState == TraversalRequestState::Cancelled && request->mPreparationOperation)
			{
				if (auto resource = mWorld.mTraversalResources.find(request->mResource);
					resource && resource->mActivePreparation)
				{
					if (auto interaction = mWorld.mInteractionRequests.find(resource->mActivePreparation))
					{
						for (auto const& [operationId, requirement] : interaction->mOperations)
						{
							(void)requirement;
							cancelDeviceOperation(operationId, request->mOwner);
						}
					}
				}
				else
				{
					cancelDeviceOperation(request->mPreparationOperation, request->mOwner);
				}
			}
		}

		if (auto permit = mWorld.mTraversalPermits.find(permitId))
		{
			auto snapshot = makeTraversalPermitSnapshot(permitId, *permit);
			mWorld.mTraversalPermits.remove(permitId);
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::TraversalPermitRemoved;
			event.phase = mWorld.mCurrentPhase;
			event.traversalPermit = std::move(snapshot);
			mWorld.mEvents.push_back(std::move(event));
		}

		if (auto request = mWorld.mTraversalRequests.find(requestId))
		{
			auto snapshot = makeTraversalRequestSnapshot(requestId, *request);
			mWorld.mTraversalRequests.remove(requestId);
			SimulationEvent event;
			event.sequence = mWorld.mNextEventSequence++;
			event.tick = mWorld.mSimulationTick;
			event.type = SimulationEventType::TraversalRequestRemoved;
			event.phase = mWorld.mCurrentPhase;
			event.traversalRequest = std::move(snapshot);
			mWorld.mEvents.push_back(std::move(event));
		}
	}

} // core
