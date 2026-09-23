#include <algorithm>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/World.h"
#include "core/Coordination.h"
#include "core/Defines.h"
#include "core/Lift.h"


namespace core
{

	using namespace std;

	// Open platform lift traversal allocation moved out of World
	// (ADR 0004 stage 3). The behaviour is unchanged: the coordinator works on
	// World's traversal-resource, traversal-request, interaction-request,
	// device-operation and sector registries through friendship, calls its own
	// lift scheduling and interaction helpers, and calls back through the
	// World facade for the machinery which has not moved out of World
	// yet - grants and denials, queue ticket attachment, and queue position
	// refresh.
	//
	// An open platform lift has no landing Door resource: the journey edge's own
	// resource is the traversal resource, and the platform is crossed through a
	// virtual boundary. Calling the platform is a physical interaction with the
	// landing call control; boarding happens only from the request's reserved
	// queue position, before the boarding cutoff, in a direction the car will
	// honour. An already-riding passenger selects an onboard destination through
	// the in-car selector and leaves through the same virtual boundary.
	//
	// The admission release which undoes this allocation - the ticket #70
	// extraction - already lives in SimulationCoordinatorLifts.cpp; the two
	// halves of the open platform lift seam now sit on the same side of the
	// facade.

	void SimulationCoordinator::allocateOpenPlatformLiftTraversal(TraversalRequestId requestId, TraversalResource& resource)
	{
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		// Legacy platform topology contains co-located mount edges around each
		// stop. The virtual boundary is owned by the journey edge, so these adapters
		// grant immediately and cannot independently admit a passenger.
		if (request->mEdgeType != EdgeType::Lift)
		{
			grantTraversalRequest(requestId);
			return;
		}
		if (!resource.mEnabled
			&& find(resource.mOccupants.begin(), resource.mOccupants.end(), request->mOwner)
				== resource.mOccupants.end())
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
			return;
		}
		auto origin = findLiftStop(resource, request->mSourceEndpoint);
		auto destination = findLiftStop(resource, request->mDestinationEndpoint);
		auto existingIntent = resource.mLiftTripIntents.find(request->mOwner);
		if (existingIntent != resource.mLiftTripIntents.end())
			destination = existingIntent->second.destinationStop;
		else if (auto actor = mWorld.mAgents.find(request->mOwner))
		{
			auto journeyDestination = findAgentLiftDestination(*actor, resource);
			if (journeyDestination < resource.mLiftStops.size()) destination = journeyDestination;
		}
		if (origin >= resource.mLiftStops.size() || destination >= resource.mLiftStops.size()
			|| origin == destination)
		{
			denyTraversalRequest(requestId);
			return;
		}
		auto occupant = find(resource.mOccupants.begin(), resource.mOccupants.end(), request->mOwner);
		if (occupant == resource.mOccupants.end())
		{
			auto actor = mWorld.mAgents.find(request->mOwner);

			// A reserved slot means this passenger is already walking through the
			// platform's single virtual crossing lane. Keep that transfer alive even
			// after the exact boarding cutoff closes to new admissions; the scheduler
			// interlocks departure on the boundary owner.
			if (request->mCapacityPosition != ~0u)
			{
				auto const position = request->mCapacityPosition;
				if (!actor || position >= resource.mCapacityPositions.size()
					|| resource.mAdmissionReservations[position] != requestId
					|| resource.mVirtualBoundaryOwners.empty()
					|| resource.mVirtualBoundaryOwners.front() != requestId)
				{
					denyTraversalRequest(requestId);
					return;
				}
				auto location = mWorld.mSectors[(size_t)resource.mLiftSector.value - 1].get();
				auto target = location->getPosition() + resource.mCapacityPositions[position];
				auto const crossingCenter = resource.mLift->getPosition().x
					+ resource.mLift->getSize().x * 0.5f;
				auto const crossingHalfWidth = CORE_PLATFORM_LIFT_CROSSING_HALF_WIDTH(
					resource.mLift->getSize().x);
				target.x = clamp(target.x, crossingCenter - crossingHalfWidth,
					crossingCenter + crossingHalfWidth);
				target.y = resource.mLiftPosition;
				actor->mTraversalLocalGoal = target;
				if (actor->getGlobalPosition().distanceTo(target) > 0.001f) return;

				resource.mAdmissionReservations[position] = {};
				resource.mOccupants[position] = request->mOwner;
				resource.mVirtualBoundaryOwners.front() = {};
				request->mCapacityPosition = ~0u;
				actor->mTraversalLocalGoal.reset();
				resource.mLiftPassenger = {};
				for (auto passenger : resource.mOccupants)
					if (passenger) { resource.mLiftPassenger = passenger; break; }
				return;
			}

			if (!request->mQueueTicket)
			{
				request->mQueueTicket = QueueTicketId{ mWorld.mNextQueueTicketValue++ };
				request->mQueuedAtTick = mWorld.mSimulationTick;
				resource.mAdmissionQueue.push_back(requestId);
				resource.mLiftTripIntents[request->mOwner] = { origin, destination, mWorld.mSimulationTick };
			}
			if (!request->mPreparationRequested)
			{
				auto control = resource.mLiftStops[origin].callControl;
				if (!control) { denyTraversalRequest(requestId, TraversalFailureReason::NoReachableControl); return; }
				auto interactionId = requestInteractionForTraversal(control, request->mOwner);
				if (!interactionId) return;
				auto interaction = mWorld.mInteractionRequests.find(interactionId);
				request->mPreparationRequested = true;
				if (interaction && !interaction->mOperations.empty())
					request->mPreparationOperation = interaction->mOperations.front().first;
				return;
			}
			auto operation = mWorld.mDeviceOperations.find(request->mPreparationOperation);
			if (!operation || operation->mState == DeviceOperationState::Pending
				|| operation->mState == DeviceOperationState::Running) return;
			if (operation->mState != DeviceOperationState::Succeeded)
			{ denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed); return; }

			// Calling the platform establishes logical priority; after the physical
			// interaction completes, join this stop's ordinary reserved-position lane.
			if (request->mQueueApproach == ~0u) attachQueueTicket(requestId, resource);
			if (request->mQueueApproach >= resource.mQueueLanes.size()
				|| request->mQueuePosition == ~0u) return;
			auto const& queueLane = resource.mQueueLanes[request->mQueueApproach];
			if (request->mQueuePosition >= queueLane.positions.size() || !actor
				|| actor->getGlobalPosition().distanceTo(
					queueLane.positions[request->mQueuePosition]) > 0.001f) return;

			// Boarding admission claims the virtual crossing lane. A passenger that has
			// not reached its queue head by the exact cutoff remains queued for the next
			// visit; an admitted passenger walks through the lane before becoming an occupant.
			if (resource.mLiftMoving || resource.mLiftCurrentStop != origin
				|| resource.mLiftStopPhase != LiftStopPhase::Boarding
				|| mWorld.mSimulationTick >= resource.mLiftBoardingCutoffTick
				|| !isLiftBoardingDirectionCompatible(resource, origin, destination)) return;
			auto selected = find_if(resource.mAdmissionQueue.begin(), resource.mAdmissionQueue.end(),
				[&](TraversalRequestId candidateId)
				{
					auto candidate = mWorld.mTraversalRequests.find(candidateId);
					if (!candidate) return false;
					auto intent = resource.mLiftTripIntents.find(candidate->mOwner);
					if (intent == resource.mLiftTripIntents.end()
						|| intent->second.originStop != origin) return false;
					auto desired = resource.mLiftStops[intent->second.destinationStop].globalPosition
						> resource.mLiftStops[origin].globalPosition
						? TraversalDirection::Ascending : TraversalDirection::Descending;
					return desired == resource.mLiftDirection;
				});
			if (selected == resource.mAdmissionQueue.end() || *selected != requestId) return;

			if (resource.mVirtualBoundaryOwners.empty()
				|| resource.mVirtualBoundaryOwners.front()) return;
			uint32_t capacityPosition = ~0u;
			for (uint32_t i = 0; i < resource.mOccupants.size(); ++i)
				if (!resource.mOccupants[i] && !resource.mAdmissionReservations[i])
				{ capacityPosition = i; break; }
			if (capacityPosition == ~0u) return;

			// Claim the lane and capacity before releasing the queue position. The
			// Agent then crosses at ordinary walking speed to the same separated
			// standing position used by a normal Lift.
			resource.mAdmissionReservations[capacityPosition] = requestId;
			resource.mVirtualBoundaryOwners.front() = requestId;
			request->mCapacityPosition = capacityPosition;
			resource.mAdmissionQueue.erase(remove(resource.mAdmissionQueue.begin(),
				resource.mAdmissionQueue.end(), requestId), resource.mAdmissionQueue.end());
			auto& laneQueue = resource.mQueueLanes[request->mQueueApproach].queue;
			laneQueue.erase(remove(laneQueue.begin(), laneQueue.end(), requestId), laneQueue.end());
			request->mQueuePosition = ~0u;
			request->mPreparationRequested = false;
			request->mPreparationOperation = {};

			auto location = mWorld.mSectors[(size_t)resource.mLiftSector.value - 1].get();
			auto target = location->getPosition() + resource.mCapacityPositions[capacityPosition];
			target.y = resource.mLiftPosition;
			actor->mTraversalLocalGoal = target;
			refreshQueuePositions(resource);
			return;
		}

		if (!resource.mLiftPassengerDestinations.contains(request->mOwner))
		{
			if (!request->mPreparationRequested)
			{
				if (destination >= resource.mControls.size()) { denyTraversalRequest(requestId); return; }
				resource.mLiftSelector = resource.mControls[destination];
				if (auto selector = mWorld.mInteractionPoints.find(resource.mLiftSelector))
					if (auto actor = mWorld.mAgents.find(request->mOwner)) selector->mPosition = actor->getGlobalPosition();
				auto interactionId = requestInteractionForTraversal(resource.mLiftSelector, request->mOwner);
				if (!interactionId) return;
				auto interaction = mWorld.mInteractionRequests.find(interactionId);
				request->mPreparationRequested = true;
				if (interaction && !interaction->mOperations.empty())
					request->mPreparationOperation = interaction->mOperations.front().first;
				return;
			}
			auto operation = mWorld.mDeviceOperations.find(request->mPreparationOperation);
			if (!operation || operation->mState == DeviceOperationState::Pending
				|| operation->mState == DeviceOperationState::Running) return;
			if (operation->mState != DeviceOperationState::Succeeded)
			{
				if (request->mPreparationAttempts++ < mWorld.mTraversalWaitingPolicy.maximumDestinationRetries)
				{ request->mPreparationRequested = false; request->mPreparationOperation = {}; return; }
				requestLiftPassengerSafeExit(request->mOwner, TraversalFailureReason::PreparationFailed);
				denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
				return;
			}
			resource.mLiftPassengerDestinations[request->mOwner] = destination;
			resource.mLiftPassenger = request->mOwner;
			resource.mLiftDestinationStop = destination;
			addLiftStopRequest(resource, destination, request->mOwner);
			return;
		}

		if (resource.mLiftMoving || resource.mLiftCurrentStop != destination
			|| (resource.mLiftStopPhase != LiftStopPhase::Disembarking
				&& resource.mLiftStopPhase != LiftStopPhase::Boarding)) return;
		resource.mLiftStopPhase = LiftStopPhase::Disembarking;
		if (!resource.mVirtualBoundaryOwners.front())
			resource.mVirtualBoundaryOwners.front() = requestId;
		if (resource.mVirtualBoundaryOwners.front() == requestId) grantTraversalRequest(requestId);
	}

} // core
