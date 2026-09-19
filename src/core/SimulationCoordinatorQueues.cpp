#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/Building.h"
#include "core/Coordination.h"
#include "core/Edge.h"
#include "core/ExtensibleObject.h"
#include "core/ForceBridge.h"
#include "core/Ladder.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/Vertex.h"


namespace core
{

	using namespace std;

	// The queue side of the queue-and-admission core moved out of Building
	// (ADR 0004 stage 4). Traversal-request creation, queue tickets, queue
	// positions and their refresh, the door queue grant and release, traversal
	// progress and timeouts, and permit expiry all live here now.
	//
	// The behaviour is unchanged. The coordinator works on Building's
	// traversal-request, traversal-permit and agent registries through
	// friendship (ADR 0001 keeps the registries with Building), and calls back
	// through the Building facade (design pattern, not the Facade sector type)
	// only for what has not moved out of Building yet - the request and permit
	// snapshot builders which publish lifecycle events.

	TraversalRequestId SimulationCoordinator::createTraversalRequest(Agent const& agent, shared_ptr<const Edge> const& edge,
		shared_ptr<const Vertex> const& source, shared_ptr<const Vertex> const& destination)
	{
		auto owner = getAgentId(&agent);
		if (!owner || !edge || !source || !destination)
		{
			throw invalid_argument("A traversal request requires an owned Agent, Edge, and two endpoints");
		}

		auto sourceSector = SectorId{ (uint64_t)source->getSector()->getIndex() + 1 };
		auto destinationSector = SectorId{ (uint64_t)destination->getSector()->getIndex() + 1 };
		auto id = mBuilding.mTraversalRequests.add(unique_ptr<TraversalRequest>(new TraversalRequest(owner,
			edge->getType(), sourceSector, destinationSector, source->getPosition(), destination->getPosition())));
		auto request = mBuilding.mTraversalRequests.find(id);
		request->mResource = edge->getTraversalResourceId();
		request->mPreferredQueueSide = agent.mEarlyQueueApproachDirectionX;
		request->mQueueSelectionPosition = request->mPreferredQueueSide
			? agent.getGlobalPosition() : agent.mPathStartPosition;
		if (!request->mPreferredQueueSide && agent.mPath.path && agent.mPath.targetNode > 0
			&& agent.mPath.targetNode - 1 < agent.mPath.path->nodes.size())
		{
			auto const& previous = agent.mPath.path->nodes[agent.mPath.targetNode - 1].targetVertex;
			if (previous) request->mQueueSelectionPosition = previous->getPosition();
		}
		if (auto resource = mBuilding.mTraversalResources.find(request->mResource); resource)
		{
			if (resource->mExtensible && resource->mExtensionRequestLeases.insert(id).second)
				resource->mExtensible->acquireExtensionLease();
			if ((resource->mDoor && !resource->mLiftCoordinator) || resource->mForceBridge)
				attachQueueTicket(id, *resource);
			else if ((resource->mLadder || resource->mStairwell)
				&& isLadderAdmission(*request, *resource))
				attachLadderAdmissionRequest(id, *resource);
		}

		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::TraversalRequestAdded;
		event.phase = mBuilding.mCurrentPhase;
		event.traversalRequest = mBuilding.makeTraversalRequestSnapshot(id, *mBuilding.mTraversalRequests.find(id));
		mBuilding.mEvents.push_back(std::move(event));
		return id;
	}

	void SimulationCoordinator::attachQueueTicket(TraversalRequestId requestId, TraversalResource& resource)
	{
		auto request = mBuilding.mTraversalRequests.find(requestId);
		if (!request || (request->mQueueTicket && request->mQueueApproach != ~0u))
		{
			return;
		}
		uint32_t approach = ~0u;
		float closestEndpoint = numeric_limits<float>::max();
		for (uint32_t i = 0; i < resource.mQueueLanes.size(); ++i)
		{
			auto const& lane = resource.mQueueLanes[i];
			if (lane.sector != request->mSourceSector) continue;
			auto const distance = lane.origin.distanceTo(request->mSourceEndpoint);
			if (distance < closestEndpoint)
			{
				approach = i;
				closestEndpoint = distance;
			}
		}
		if (approach == ~0u) return;
		if (!request->mQueueTicket)
		{
			request->mQueueTicket = QueueTicketId{ mBuilding.mNextQueueTicketValue++ };
			request->mQueuedAtTick = mBuilding.mSimulationTick;
		}
		request->mQueueApproach = approach;
		auto& queue = resource.mQueueLanes[approach].queue;
		if (find(queue.begin(), queue.end(), requestId) == queue.end()) queue.push_back(requestId);
		sort(queue.begin(), queue.end(), [&](auto left, auto right)
		{
			auto lhs = mBuilding.mTraversalRequests.find(left);
			auto rhs = mBuilding.mTraversalRequests.find(right);
			return lhs && rhs ? lhs->mQueueTicket < rhs->mQueueTicket : left < right;
		});
		refreshQueuePositions(resource);
	}

	bool SimulationCoordinator::stopForAvailableQueuePosition(Agent& agent,
		shared_ptr<const Edge> const& edge, Vector2 const& endpoint,
		float movementDistance)
	{
		if (!edge || movementDistance < 0.0f || !agent.getSector()) return false;
		auto resource = mBuilding.mTraversalResources.find(edge->getTraversalResourceId());
		if (!resource || (!resource->mDoor && !resource->mLadder && !resource->mForceBridge
			&& !resource->mOpenPlatformLift)) return false;

		auto const sourceSector = SectorId{ (uint64_t)agent.getSector()->getIndex() + 1 };
		QueueLane const* lane = nullptr;
		float closestEndpoint = numeric_limits<float>::max();
		for (auto const& candidate : resource->mQueueLanes)
		{
			if (candidate.sector != sourceSector) continue;
			auto const distance = candidate.origin.distanceTo(endpoint);
			if (distance < closestEndpoint)
			{
				lane = &candidate;
				closestEndpoint = distance;
			}
		}
		if (!lane || lane->positions.empty()) return false;

		auto const position = agent.getGlobalPosition();
		int const direction = position.x < lane->origin.x - 0.001f ? 1
			: position.x > lane->origin.x + 0.001f ? -1 : 0;
		if (direction == 0) return false;

		bool hasOccupiedPosition = false;
		bool hasAvailablePosition = false;
		float availablePosition = direction > 0
			? numeric_limits<float>::lowest() : numeric_limits<float>::max();
		for (uint32_t i = 0; i < lane->positions.size(); ++i)
		{
			auto const& queuePosition = lane->positions[i];
			auto const onApproachSide = direction > 0
				? queuePosition.x <= lane->origin.x + 0.001f
				: queuePosition.x >= lane->origin.x - 0.001f;
			auto const notBehindAgent = direction > 0
				? queuePosition.x >= position.x - 0.001f
				: queuePosition.x <= position.x + 0.001f;
			if (!onApproachSide || !notBehindAgent) continue;
			if (i < lane->positionOwners.size() && lane->positionOwners[i])
			{
				hasOccupiedPosition = true;
				continue;
			}
			// Choose the available spot nearest the endpoint: with compact queue
			// assignment this is immediately outside the occupied tail.
			hasAvailablePosition = true;
			if (direction > 0) availablePosition = max(availablePosition, queuePosition.x);
			else availablePosition = min(availablePosition, queuePosition.x);
		}

		bool ladderCannotAdmitImmediately = false;
		if (resource->mLadder)
		{
			bool noCapacity = true;
			for (uint32_t i = 0; i < resource->mCapacity; ++i)
				noCapacity = noCapacity
					&& (resource->mOccupants[i] || resource->mAdmissionReservations[i]);
			auto const approachingDirection = endpoint.y
				< resource->mLadder->getPosition().y + resource->mLadder->getSize().y * 0.5f
				? TraversalDirection::Ascending : TraversalDirection::Descending;
			ladderCannotAdmitImmediately = noCapacity
				|| (resource->mActiveDirection != TraversalDirection::None
					&& resource->mActiveDirection != approachingDirection);
		}

		// With no established queue, an Agent proceeds to an available threshold.
		// An existing Door/Lift tail, or unavailable Ladder admission, claims the
		// nearest forward spot before the Agent reaches it.
		if (!hasAvailablePosition
			|| (!hasOccupiedPosition && !ladderCannotAdmitImmediately)) return false;
		auto const reachesQueue = direction > 0
			? position.x + movementDistance >= availablePosition - 0.001f
			: position.x - movementDistance <= availablePosition + 0.001f;
		if (!reachesQueue) return false;

		agent.mEarlyQueueApproachDirectionX = direction;
		return true;
	}

	void SimulationCoordinator::refreshQueuePositions(TraversalResource& resource)
	{
		// Doors and Ladders intentionally share this allocator: prefer proximity
		// to the resource endpoint, then proximity to the waiting Agent.
		for (auto& lane : resource.mQueueLanes)
		{
			map<TraversalRequestId, uint32_t> previousPositions;
			for (uint32_t i = 0; i < lane.positionOwners.size(); ++i)
			{
				if (lane.positionOwners[i]) previousPositions[lane.positionOwners[i]] = i;
			}
			fill(lane.positionOwners.begin(), lane.positionOwners.end(), TraversalRequestId{});
			for (auto requestId : lane.queue)
			{
				auto request = mBuilding.mTraversalRequests.find(requestId);
				if (!request || request->mState != TraversalRequestState::Pending)
				{
					continue;
				}
				request->mQueuePosition = ~0u;
				if (auto agent = mBuilding.mAgents.find(request->mOwner)) agent->mTraversalLocalGoal.reset();

				// Operators and timed-out assignments keep their logical place while
				// releasing the scarce physical position.
				if (resource.mPreparationOperator == requestId
					|| mBuilding.mSimulationTick < request->mPositionRetryAtTick)
				{
					continue;
				}
				auto agent = mBuilding.mAgents.find(request->mOwner);
				auto const selectionPosition = request->mHasHeldQueuePosition && agent
					? agent->getGlobalPosition() : request->mQueueSelectionPosition;
				uint32_t position = ~0u;
				float bestObjectDistance = numeric_limits<float>::max();
				float bestAgentDistance = numeric_limits<float>::max();
				for (uint32_t candidate = 0; candidate < lane.positionOwners.size(); ++candidate)
				{
					if (lane.positionOwners[candidate]) continue;
					if ((request->mPreferredQueueSide > 0
							&& (lane.positions[candidate].x > lane.origin.x + 0.001f
								|| lane.positions[candidate].x
									< request->mQueueSelectionPosition.x - 0.001f))
						|| (request->mPreferredQueueSide < 0
							&& (lane.positions[candidate].x < lane.origin.x - 0.001f
								|| lane.positions[candidate].x
									> request->mQueueSelectionPosition.x + 0.001f)))
						continue;
					auto objectDistance = lane.positions[candidate].distanceTo(request->mSourceEndpoint);
					auto agentDistance = lane.positions[candidate].distanceTo(selectionPosition);
					if (objectDistance < bestObjectDistance - 0.001f
						|| (abs(objectDistance - bestObjectDistance) <= 0.001f
							&& agentDistance < bestAgentDistance - 0.001f))
					{
						position = candidate;
						bestObjectDistance = objectDistance;
						bestAgentDistance = agentDistance;
					}
				}
				if (position != ~0u)
				{
					lane.positionOwners[position] = requestId;
					request->mQueuePosition = position;
					request->mHasHeldQueuePosition = true;
					if (agent)
					{
						if (!resource.mOpenPlatformMissedBoarding.contains(requestId))
							agent->mTraversalLocalGoal = lane.positions[position];
						auto distance = agent->getGlobalPosition().distanceTo(lane.positions[position]);
						auto previous = previousPositions.find(requestId);
						if (previous == previousPositions.end() || previous->second != position)
						{
							request->mPositionAssignedAtTick = mBuilding.mSimulationTick;
							request->mLastPositionProgressTick = mBuilding.mSimulationTick;
							request->mBestPositionDistance = distance;
						}
					}
				}
			}
		}
	}

	void SimulationCoordinator::updateTraversalProgressAndTimeouts()
	{
		vector<TraversalPermitId> expiredPermits;
		for (auto const& [permitId, permit] : mBuilding.mTraversalPermits.entries())
		{
			if (permit->mState != TraversalPermitState::Active) continue;
			auto request = mBuilding.mTraversalRequests.find(permit->mRequest);
			auto agent = request ? mBuilding.mAgents.find(request->mOwner) : nullptr;
			if (!request || !agent) { expiredPermits.push_back(permitId); continue; }
			auto distance = agent->getGlobalPosition().distanceTo(request->mDestinationEndpoint);
			if (distance + 0.001f < permit->mBestDestinationDistance)
			{
				permit->mBestDestinationDistance = distance;
				permit->mExpiresAtTick = mBuilding.mSimulationTick + mBuilding.mTraversalWaitingPolicy.permitProgressTimeoutTicks;
			}
			else if (mBuilding.mSimulationTick >= permit->mExpiresAtTick)
			{
				expiredPermits.push_back(permitId);
			}
		}
		for (auto permitId : expiredPermits) expireTraversalPermit(permitId);

		vector<TraversalRequestId> unreachableRequests;
		for (auto const& [resourceId, resource] : mBuilding.mTraversalResources.entries())
		{
			(void)resourceId;
			if (none_of(resource->mQueueLanes.begin(), resource->mQueueLanes.end(),
				[](auto const& lane) { return (bool)lane.sector; })) continue;
			bool refresh = false;
			for (auto& lane : resource->mQueueLanes)
			{
				for (auto requestId : lane.queue)
				{
					if (resource->mOpenPlatformMissedBoarding.contains(requestId)) continue;
					auto request = mBuilding.mTraversalRequests.find(requestId);
					if (!request) continue;
					if (request->mQueuePosition == ~0u)
					{
						if (request->mPositionRetryAtTick != 0
							&& mBuilding.mSimulationTick >= request->mPositionRetryAtTick)
						{
							request->mPositionRetryAtTick = 0;
							refresh = true;
						}
						continue;
					}
					auto agent = mBuilding.mAgents.find(request->mOwner);
					if (!agent || request->mQueuePosition >= lane.positions.size()) continue;
					auto distance = agent->getGlobalPosition().distanceTo(lane.positions[request->mQueuePosition]);
					if (distance + 0.001f < request->mBestPositionDistance)
					{
						request->mBestPositionDistance = distance;
						request->mLastPositionProgressTick = mBuilding.mSimulationTick;
					}
					else if (distance > 0.001f && mBuilding.mSimulationTick - request->mLastPositionProgressTick
						>= mBuilding.mTraversalWaitingPolicy.localGoalTimeoutTicks)
					{
						request->mQueuePosition = ~0u;
						request->mPositionRetryAtTick = mBuilding.mSimulationTick
							+ mBuilding.mTraversalWaitingPolicy.localGoalRetryDelayTicks;
						++request->mPositionRetryCount;
						if (agent) agent->mTraversalLocalGoal.reset();
						refresh = true;
						if (request->mPositionRetryCount > mBuilding.mTraversalWaitingPolicy.maximumLocalGoalRetries)
						{
							unreachableRequests.push_back(requestId);
						}
					}
				}
			}
			if (refresh) refreshQueuePositions(*resource);
		}
		for (auto requestId : unreachableRequests)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::LocalGoalUnreachable);
		}
	}

	void SimulationCoordinator::expireTraversalPermit(TraversalPermitId permitId)
	{
		auto permit = mBuilding.mTraversalPermits.find(permitId);
		if (!permit || permit->mState != TraversalPermitState::Active) return;
		auto requestId = permit->mRequest;
		auto request = mBuilding.mTraversalRequests.find(requestId);
		if (!request) return;

		permit->mState = TraversalPermitState::Cancelled;
		SimulationEvent permitChanged;
		permitChanged.sequence = mBuilding.mNextEventSequence++;
		permitChanged.tick = mBuilding.mSimulationTick;
		permitChanged.type = SimulationEventType::TraversalPermitChanged;
		permitChanged.phase = mBuilding.mCurrentPhase;
		permitChanged.traversalPermit = mBuilding.makeTraversalPermitSnapshot(permitId, *permit);
		mBuilding.mEvents.push_back(std::move(permitChanged));

		request->mPermit = {};
		request->mState = TraversalRequestState::Pending;
		request->mFailureReason = TraversalFailureReason::PermitExpired;
		if (auto resource = mBuilding.mTraversalResources.find(request->mResource);
			resource && (resource->mLadder || resource->mStairwell))
		{
			if (isLadderAdmission(*request, *resource))
			{
				releaseLadderAdmission(requestId, *resource);
				attachLadderAdmissionRequest(requestId, *resource);
			}
		}
		else if (auto resource = mBuilding.mTraversalResources.find(request->mResource);
			resource && resource->mDoor && resource->mLiftCoordinator)
		{
			for (auto& owner : resource->mCrossingOwners) if (owner == requestId) owner = {};
			if (!request->mPreparationLease)
				request->mPreparationLease = acquireDoorOpenLease(*resource,
					DoorOpenLeaseKind::Preparation, requestId);
			if (request->mCrossingLease) releaseDoorOpenLease(*resource, request->mCrossingLease);
			request->mCrossingLease = {};
			request->mCrossingLane = ~0u;
			auto coordinator = mBuilding.mTraversalResources.find(resource->mLiftCoordinator);
			if (coordinator && request->mSourceSector != coordinator->mLiftSector)
			{
				for (uint32_t approach = 0; approach < resource->mQueueLanes.size(); ++approach)
				{
					auto& queueLane = resource->mQueueLanes[approach];
					if (queueLane.sector != request->mSourceSector) continue;
					request->mQueueApproach = approach;
					if (find(queueLane.queue.begin(), queueLane.queue.end(), requestId) == queueLane.queue.end())
						queueLane.queue.push_back(requestId);
					sort(queueLane.queue.begin(), queueLane.queue.end(), [&](auto left, auto right)
					{
						auto lhs = mBuilding.mTraversalRequests.find(left);
						auto rhs = mBuilding.mTraversalRequests.find(right);
						return lhs && rhs ? lhs->mQueueTicket < rhs->mQueueTicket : left < right;
					});
					refreshQueuePositions(*resource);
					break;
				}
			}
		}
		else if (auto resource = mBuilding.mTraversalResources.find(request->mResource);
			resource && (resource->mDoor || resource->mForceBridge))
		{
			for (auto& owner : resource->mCrossingOwners) if (owner == requestId) owner = {};
			// Downgrade Door crossing authority to preparation before releasing its
			// safety lease. Force Bridges remain extended through their request lease.
			if (resource->mDoor && !request->mPreparationLease && resource->mEnabled)
			{
				request->mPreparationLease = acquireDoorOpenLease(*resource,
					DoorOpenLeaseKind::Preparation, requestId);
			}
			if (resource->mDoor && request->mCrossingLease)
				releaseDoorOpenLease(*resource, request->mCrossingLease);
			request->mCrossingLease = {};
			request->mCrossingLane = ~0u;
			auto& queue = resource->mQueueLanes[request->mQueueApproach].queue;
			queue.push_back(requestId);
			sort(queue.begin(), queue.end(), [&](TraversalRequestId lhs, TraversalRequestId rhs)
			{
				return mBuilding.mTraversalRequests.find(lhs)->mQueueTicket < mBuilding.mTraversalRequests.find(rhs)->mQueueTicket;
			});
			refreshQueuePositions(*resource);
		}
		if (auto agent = mBuilding.mAgents.find(request->mOwner); agent && agent->mTraversalTask)
		{
			agent->mTraversalTask->permit = {};
			agent->mState = Agent::State::WaitingForTraversal;
		}

		auto removed = mBuilding.makeTraversalPermitSnapshot(permitId, *permit);
		mBuilding.mTraversalPermits.remove(permitId);
		SimulationEvent permitRemoved;
		permitRemoved.sequence = mBuilding.mNextEventSequence++;
		permitRemoved.tick = mBuilding.mSimulationTick;
		permitRemoved.type = SimulationEventType::TraversalPermitRemoved;
		permitRemoved.phase = mBuilding.mCurrentPhase;
		permitRemoved.traversalPermit = std::move(removed);
		mBuilding.mEvents.push_back(std::move(permitRemoved));

		SimulationEvent requestChanged;
		requestChanged.sequence = mBuilding.mNextEventSequence++;
		requestChanged.tick = mBuilding.mSimulationTick;
		requestChanged.type = SimulationEventType::TraversalRequestChanged;
		requestChanged.phase = mBuilding.mCurrentPhase;
		requestChanged.traversalRequest = mBuilding.makeTraversalRequestSnapshot(requestId, *request);
		mBuilding.mEvents.push_back(std::move(requestChanged));
	}

	void SimulationCoordinator::tryGrantDoorQueue(TraversalResource& resource)
	{
		if (!resource.mEnabled || (!resource.mDoor && !resource.mForceBridge)
			|| (resource.mDoor && !resource.mDoor->isOpen())
			|| (resource.mForceBridge && !resource.mForceBridge->isExtended()))
		{
			return;
		}

		bool queueChanged = false;
		for (uint32_t crossingLane = 0; crossingLane < resource.mCrossingOwners.size(); ++crossingLane)
		{
			if (resource.mCrossingOwners[crossingLane])
			{
				continue;
			}
			TraversalRequestId selected;
			for (auto const& lane : resource.mQueueLanes)
			{
				if (lane.queue.empty()) continue;
				auto candidateId = lane.queue.front();
				auto candidate = mBuilding.mTraversalRequests.find(candidateId);
				if (!candidate || candidate->mState != TraversalRequestState::Pending
					|| candidate->mQueuePosition == ~0u || resource.mPreparationOperator == candidateId)
				{
					continue;
				}
				auto agent = mBuilding.mAgents.find(candidate->mOwner);
				if (!agent || agent->getGlobalPosition().distanceTo(
					lane.positions[candidate->mQueuePosition]) > 0.001f)
				{
					continue;
				}
				if (!selected)
				{
					selected = candidateId;
					continue;
				}
				auto current = mBuilding.mTraversalRequests.find(selected);
				if (candidate->mQueuedAtTick < current->mQueuedAtTick
					|| (candidate->mQueuedAtTick == current->mQueuedAtTick && candidate->mOwner < current->mOwner))
				{
					selected = candidateId;
				}
			}
			if (!selected) break;

			auto selectedRequest = mBuilding.mTraversalRequests.find(selected);
			auto& queueLane = resource.mQueueLanes[selectedRequest->mQueueApproach];
			queueLane.queue.erase(remove(queueLane.queue.begin(), queueLane.queue.end(), selected), queueLane.queue.end());
			selectedRequest->mQueuePosition = ~0u;
			selectedRequest->mCrossingLane = crossingLane;
			resource.mCrossingOwners[crossingLane] = selected;
			if (auto agent = mBuilding.mAgents.find(selectedRequest->mOwner)) agent->mTraversalLocalGoal.reset();
			queueChanged = true;
			grantTraversalRequest(selected);
		}
		if (queueChanged) refreshQueuePositions(resource);
	}

	void SimulationCoordinator::releaseDoorQueueOwnership(TraversalRequestId requestId, TraversalResource& resource)
	{
		for (auto& lane : resource.mQueueLanes)
		{
			lane.queue.erase(remove(lane.queue.begin(), lane.queue.end(), requestId), lane.queue.end());
		}
		for (auto& owner : resource.mCrossingOwners)
		{
			if (owner == requestId) owner = {};
		}
		if (auto request = mBuilding.mTraversalRequests.find(requestId))
		{
			request->mQueuePosition = ~0u;
			request->mCrossingLane = ~0u;
			if (auto agent = mBuilding.mAgents.find(request->mOwner))
			{
				agent->mTraversalLocalGoal.reset();
			}
		}
		if (resource.mPreparationOperator == requestId)
			resource.mPreparationOperator = {};
		refreshQueuePositions(resource);
	}

} // core
