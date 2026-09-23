#include <algorithm>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/World.h"
#include "core/Coordination.h"
#include "core/Defines.h"
#include "core/ExtensibleObject.h"
#include "core/Ladder.h"
#include "core/Stairwell.h"


namespace core
{

	using namespace std;

	// The ladder admission half of the queue-and-admission core moved out of
	// World (ADR 0004 stage 4): which requests are admission-controlled,
	// how they join the deterministic admission queue, the entry-spacing rule
	// which keeps climbers from overlapping on the span, the directional
	// batch grant, and the release of admission and occupancy.
	//
	// The behaviour is unchanged. The coordinator works on World's
	// traversal-request and agent registries through friendship and calls its
	// own queue refresh, grant and denial machinery directly, so no facade
	// callback (design pattern, not the Facade sector type) is needed in this
	// seam.

	bool SimulationCoordinator::isLadderAdmission(TraversalRequest const& request,
		TraversalResource const& resource) const
	{
		// Only the edge that claims climbing capacity is admission-controlled.
		// Mount/dismount edges which do not put a new Agent on the climbing span
		// must remain immediately traversable or an Agent could reserve capacity twice.
		if ((!resource.mLadder && !resource.mStairwell)
			|| request.mDestinationSector != resource.mLadderSector)
		{
			return false;
		}
		if (resource.mStairwell)
		{
			// Ordinary mount edges remain unconstrained. A narrow stairwell owns
			// capacity only for the actual sloping, cross-deck edge.
			return request.mSourceSector == resource.mLadderSector
				&& request.mEdgeType == EdgeType::Stairwell;
		}
		// Entering a dedicated Ladder sector starts occupancy. For a Room Ladder,
		// whose climb remains inside one Room sector, the Ladder edge itself starts it.
		if (request.mSourceSector != resource.mLadderSector)
		{
			return true;
		}
		if (request.mEdgeType != EdgeType::Ladder)
		{
			return false;
		}
		return find(resource.mOccupants.begin(), resource.mOccupants.end(), request.mOwner)
			== resource.mOccupants.end();
	}

	void SimulationCoordinator::attachLadderAdmissionRequest(TraversalRequestId requestId,
		TraversalResource& resource)
	{
		auto request = mWorld.mTraversalRequests.find(requestId);
		if (!request) return;
		if (request->mDirection == TraversalDirection::None)
		{
			// Direction is fixed when the request joins the admission queue. Dedicated
			// Ladder sectors reveal it from the cross-deck edge; Room Ladders reveal it
			// from the side of the physical midpoint where the Agent approaches.
			if (request->mSourceSector == resource.mLadderSector)
			{
				auto deltaY = request->mDestinationEndpoint.y - request->mSourceEndpoint.y;
				request->mDirection = deltaY >= 0.0f
					? TraversalDirection::Ascending : TraversalDirection::Descending;
			}
			else
			{
				auto objectY = resource.mLadder ? resource.mLadder->getPosition().y
					: resource.mStairwell->getPosition().y;
				auto objectHeight = resource.mLadder ? resource.mLadder->getSize().y
					: resource.mStairwell->getSize().y;
				request->mDirection = request->mSourceEndpoint.y < objectY + objectHeight * 0.5f
					? TraversalDirection::Ascending : TraversalDirection::Descending;
			}
		}
		// A queue ticket gives a Ladder requester a physical place to wait. The
		// admission queue is separately sorted for deterministic first-come ordering;
		// stable ID tie-breakers make equal-tick requests independent of iteration order.
		if (resource.mLadder) attachQueueTicket(requestId, resource);
		if (find(resource.mAdmissionQueue.begin(), resource.mAdmissionQueue.end(), requestId)
			== resource.mAdmissionQueue.end())
		{
			resource.mAdmissionQueue.push_back(requestId);
			sort(resource.mAdmissionQueue.begin(), resource.mAdmissionQueue.end(),
				[&](auto lhs, auto rhs)
				{
					auto left = mWorld.mTraversalRequests.find(lhs);
					auto right = mWorld.mTraversalRequests.find(rhs);
					if (!left || !right) return lhs < rhs;
					return left->mQueuedAtTick != right->mQueuedAtTick
						? left->mQueuedAtTick < right->mQueuedAtTick
						: (left->mOwner != right->mOwner ? left->mOwner < right->mOwner : lhs < rhs);
				});
		}
	}

	bool SimulationCoordinator::ladderEntryHasClearedSpacing(TraversalResource const& resource) const
	{
		// Every climber moves at the same climb speed, so the separation established
		// when an Agent mounts persists for its whole climb. A new climber may only
		// be admitted once every in-flight Agent has cleared the entry altitude by a
		// full spacing; otherwise the newcomer catches up and overlaps on the span.
		// Narrow stairwells carry no spacing and remain governed by capacity alone.
		if (!resource.mLadder || resource.mLadderSpacing <= 0.0f) return true;
		auto const ascending = resource.mActiveDirection != TraversalDirection::Descending;
		auto const entryAltitude = resource.mLadder->getPosition().y - CORE_LADDER_HEIGHT_OFF_GROUND
			+ (ascending ? 0.0f : (float)(resource.mLadder->getDecksHigh() - 1));
		auto cleared = [&](AgentId id)
		{
			auto agent = mWorld.mAgents.find(id);
			if (!agent) return true;
			auto const progress = ascending
				? agent->getGlobalPosition().y - entryAltitude
				: entryAltitude - agent->getGlobalPosition().y;
			return progress >= resource.mLadderSpacing - 0.001f;
		};
		for (auto occupant : resource.mOccupants)
		{
			if (occupant && !cleared(occupant)) return false;
		}
		// Granted-but-not-yet-committed Agents are still walking to the mount point;
		// their zero progress correctly keeps the entry closed until they climb clear.
		for (auto reservation : resource.mAdmissionReservations)
		{
			if (!reservation) continue;
			auto request = mWorld.mTraversalRequests.find(reservation);
			if (request && !cleared(request->mOwner)) return false;
		}
		return true;
	}

	void SimulationCoordinator::tryGrantLadderAdmissions(TraversalResource& resource)
	{
		// Disabled or moving/retracted equipment cannot safely accept a new climber.
		// Existing occupants retain their ownership while the admission gate is closed.
		if (!resource.mEnabled || (!resource.mLadder && !resource.mStairwell)
			|| (resource.mExtensible && !resource.mExtensible->isExtended())) return;

		// Occupants and granted-but-not-yet-committed reservations are both in flight.
		// Direction may change only after both sets are empty, so opposite-direction
		// Agents can never meet on the climbing span.
		auto hasInFlight = any_of(resource.mOccupants.begin(), resource.mOccupants.end(),
			[](auto id) { return (bool)id; })
			|| any_of(resource.mAdmissionReservations.begin(), resource.mAdmissionReservations.end(),
				[](auto id) { return (bool)id; });
		auto oldestDirection = [&]()
		{
			for (auto requestId : resource.mAdmissionQueue)
			{
				if (auto request = mWorld.mTraversalRequests.find(requestId);
					request && request->mState == TraversalRequestState::Pending)
					return request->mDirection;
			}
			return TraversalDirection::None;
		};
		auto hasWaitingDirection = [&](TraversalDirection direction)
		{
			return any_of(resource.mAdmissionQueue.begin(), resource.mAdmissionQueue.end(),
				[&](auto id)
				{
					auto request = mWorld.mTraversalRequests.find(id);
					return request && request->mState == TraversalRequestState::Pending
						&& request->mDirection == direction;
				});
		};

		// Start with the oldest request. Once a directional batch is underway, drain
		// the span before switching. An opposite queue gets the next turn when the
		// current direction has no demand or has consumed its configured batch limit.
		if (resource.mActiveDirection == TraversalDirection::None)
		{
			resource.mActiveDirection = oldestDirection();
			resource.mDirectionalBatchCount = 0;
		}
		else if (!hasInFlight)
		{
			auto opposite = resource.mActiveDirection == TraversalDirection::Ascending
				? TraversalDirection::Descending : TraversalDirection::Ascending;
			if (hasWaitingDirection(opposite)
				&& (!hasWaitingDirection(resource.mActiveDirection)
					|| resource.mDirectionalBatchCount >= resource.mDirectionalBatchLimit))
			{
				resource.mActiveDirection = opposite;
				resource.mDirectionalBatchCount = 0;
			}
			else if (!hasWaitingDirection(resource.mActiveDirection))
			{
				resource.mActiveDirection = oldestDirection();
				resource.mDirectionalBatchCount = 0;
			}
		}

		auto opposite = resource.mActiveDirection == TraversalDirection::Ascending
			? TraversalDirection::Descending : TraversalDirection::Ascending;
		// Stop enlarging this batch when opposite demand is waiting. Current climbers
		// finish first; the empty-span rule above will then reverse the direction.
		if (hasWaitingDirection(opposite)
			&& resource.mDirectionalBatchCount >= resource.mDirectionalBatchLimit)
			return;

		// Fill every physically available spacing slot, but only with requests in the
		// active direction whose Agent has actually reached the head of its queue.
		for (uint32_t position = 0; position < resource.mCapacity; ++position)
		{
			if (resource.mOccupants[position] || resource.mAdmissionReservations[position]) continue;
			if (!ladderEntryHasClearedSpacing(resource)) break;
			auto selected = find_if(resource.mAdmissionQueue.begin(), resource.mAdmissionQueue.end(),
				[&](auto id)
				{
					auto request = mWorld.mTraversalRequests.find(id);
					if (!request || request->mState != TraversalRequestState::Pending
						|| request->mDirection != resource.mActiveDirection) return false;
					if (!resource.mLadder) return true;
					auto agent = mWorld.mAgents.find(request->mOwner);
					if (agent && request->mPreferredQueueSide == 0
						&& agent->getGlobalPosition().distanceTo(request->mSourceEndpoint) <= 0.001f)
						return true;
					if (request->mQueueApproach >= resource.mQueueLanes.size()
						|| request->mQueuePosition == ~0u) return false;
					auto const& lane = resource.mQueueLanes[request->mQueueApproach];
					return agent && request->mQueuePosition < lane.positions.size()
						&& agent->getGlobalPosition().distanceTo(
							lane.positions[request->mQueuePosition]) <= 0.001f;
				});
			if (selected == resource.mAdmissionQueue.end()) break;
			auto requestId = *selected;
			resource.mAdmissionQueue.erase(selected);
			auto request = mWorld.mTraversalRequests.find(requestId);
			if (resource.mLadder)
			{
				auto& lane = resource.mQueueLanes[request->mQueueApproach];
				lane.queue.erase(remove(lane.queue.begin(), lane.queue.end(), requestId), lane.queue.end());
				request->mQueuePosition = ~0u;
				if (auto agent = mWorld.mAgents.find(request->mOwner)) agent->mTraversalLocalGoal.reset();
				refreshQueuePositions(resource);
			}
			// Reserve before issuing the permit: movement and commit can now rely on
			// this exact slot remaining unavailable to every other Agent.
			resource.mAdmissionReservations[position] = requestId;
			request->mCapacityPosition = position;
			++resource.mDirectionalBatchCount;
			grantTraversalRequest(requestId);
			if (hasWaitingDirection(opposite)
				&& resource.mDirectionalBatchCount >= resource.mDirectionalBatchLimit) break;
		}
	}

	void SimulationCoordinator::releaseLadderAdmission(TraversalRequestId requestId,
		TraversalResource& resource)
	{
		// Cancellation, denial, or completion must surrender every form of waiting
		// ownership so neither a queue place nor a capacity reservation leaks.
		resource.mAdmissionQueue.erase(remove(resource.mAdmissionQueue.begin(),
			resource.mAdmissionQueue.end(), requestId), resource.mAdmissionQueue.end());
		for (auto& lane : resource.mQueueLanes)
			lane.queue.erase(remove(lane.queue.begin(), lane.queue.end(), requestId), lane.queue.end());
		for (auto& reservation : resource.mAdmissionReservations)
		{
			if (reservation == requestId) reservation = {};
		}
		if (auto request = mWorld.mTraversalRequests.find(requestId))
		{
			request->mCapacityPosition = ~0u;
			request->mQueuePosition = ~0u;
			if (auto agent = mWorld.mAgents.find(request->mOwner)) agent->mTraversalLocalGoal.reset();
		}
		refreshQueuePositions(resource);
	}

	void SimulationCoordinator::releaseLadderOccupancy(AgentId agentId, TraversalResource& resource)
	{
		// Occupancy lasts until the Agent leaves the climbing span, not merely until
		// its entry permit commits. Releasing it is what makes room for the next Agent.
		for (auto& occupant : resource.mOccupants)
		{
			if (occupant == agentId) occupant = {};
		}
	}

} // core
