#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/Building.h"
#include "core/Coordination.h"
#include "core/Edge.h"
#include "core/Path.h"
#include "core/Sector.h"
#include "core/Vertex.h"


namespace core
{

	using namespace std;

	// Lift scheduling, passenger safe exits and the riding and disembarking branches
	// of lift allocation moved out of Building (ADR 0004 stage 3). The behaviour is
	// unchanged: the coordinator works on Building's traversal-resource,
	// traversal-request, interaction-request and agent registries through
	// friendship, and calls back through the Building facade for the machinery
	// which has not moved out of Building yet - queue position refresh, grants and
	// denials. The shuttle passenger-carriage lookup these used to call back for
	// has since moved in with the shuttle door assignment family
	// (SimulationCoordinatorShuttles.cpp) and is called directly.
	//
	// These were helpers with no entry points of their own until the riding branch
	// of lift allocation joined them: every caller reaches the scheduling helpers
	// either from inside the coordinator or through the Building facade (design
	// pattern, not the Facade sector type), while allocateLiftRiding and
	// allocateLiftDisembarking are reached only through that facade, from
	// Building's lift allocation dispatcher.

	uint32_t SimulationCoordinator::findLiftStop(TraversalResource const& resource, Vector2 const& endpoint) const
	{
		uint32_t best = ~0u;
		float distance = 0.0f;
		for (uint32_t i = 0; i < resource.mLiftStops.size(); ++i)
		{
			auto coordinate = resource.mShuttle ? endpoint.x : endpoint.y;
			auto candidate = abs(resource.mLiftStops[i].globalPosition - coordinate);
			if (best == ~0u || candidate < distance)
			{
				best = i;
				distance = candidate;
			}
		}
		return best;
	}

	uint32_t SimulationCoordinator::findAgentLiftDestination(Agent const& agent,
		TraversalResource const& resource) const
	{
		if (!agent.mPath.path) return ~0u;
		uint32_t destination = ~0u;
		bool foundRide = false;
		for (uint32_t i = agent.mPath.targetNode + 1; i < agent.mPath.path->nodes.size(); ++i)
		{
			auto const& node = agent.mPath.path->nodes[i];
			if (!node.edge) continue;
			if ((node.edge->getType() == EdgeType::Lift
				|| node.edge->getType() == EdgeType::Shuttle) && node.targetVertex)
			{
				foundRide = true;
				destination = findLiftStop(resource, node.targetVertex->getPosition());
				continue;
			}
			// A contiguous set of ride edges is one journey. Stop at its
			// disembark edge rather than accidentally inspecting a later lift.
			if (foundRide) break;
		}
		return destination;
	}

	bool SimulationCoordinator::liftHasDisembarkDemand(TraversalResource const& resource, uint32_t stop) const
	{
		for (auto occupant : resource.mOccupants)
		{
			if (!occupant) continue;
			auto destination = resource.mLiftPassengerDestinations.find(occupant);
			if (destination != resource.mLiftPassengerDestinations.end() && destination->second == stop)
				return true;
		}
		return false;
	}

	void SimulationCoordinator::addLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner)
	{
		if (!owner || stop >= resource.mLiftStopRequestOwners.size()) return;
		if (resource.mLiftStopRequestOwners[stop].insert(owner).second)
			resource.mLiftStopRequestTicks[stop][owner] = mBuilding.mSimulationTick;
	}

	void SimulationCoordinator::removeLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner)
	{
		if (!owner || stop >= resource.mLiftStopRequestOwners.size()) return;
		resource.mLiftStopRequestOwners[stop].erase(owner);
		resource.mLiftStopRequestTicks[stop].erase(owner);
	}

	uint32_t SimulationCoordinator::chooseNextLiftStop(TraversalResource& resource) const
	{
		auto requested = [&](uint32_t stop)
		{
			return stop < resource.mLiftStopRequestOwners.size()
				&& !resource.mLiftStopRequestOwners[stop].empty();
		};
		auto position = resource.mLiftPosition;
		auto nearestInDirection = [&](TraversalDirection direction)
		{
			uint32_t selected = ~0u;
			float selectedDistance = 0.0f;
			for (uint32_t stop = 0; stop < resource.mLiftStops.size(); ++stop)
			{
				if (!requested(stop) || stop == resource.mLiftCurrentStop) continue;
				auto hasCompatibleOwner = any_of(resource.mLiftStopRequestOwners[stop].begin(),
					resource.mLiftStopRequestOwners[stop].end(), [&](AgentId owner)
					{
						if (resource.mLiftPassengerDestinations.contains(owner)) return true;
						auto intent = resource.mLiftTripIntents.find(owner);
						if (intent == resource.mLiftTripIntents.end()
							|| intent->second.destinationStop >= resource.mLiftStops.size()) return true;
						auto desired = resource.mLiftStops[intent->second.destinationStop].globalPosition
							> resource.mLiftStops[intent->second.originStop].globalPosition
							? TraversalDirection::Ascending : TraversalDirection::Descending;
						return desired == direction;
					});
				if (!hasCompatibleOwner) continue;
				auto delta = resource.mLiftStops[stop].globalPosition - position;
				if ((direction == TraversalDirection::Ascending && delta <= 0.0f)
					|| (direction == TraversalDirection::Descending && delta >= 0.0f)) continue;
				auto distance = abs(delta);
				if (selected == ~0u || distance < selectedDistance
					|| (distance == selectedDistance && stop < selected))
				{
					selected = stop;
					selectedDistance = distance;
				}
			}
			return selected;
		};

		if (resource.mLiftDirection != TraversalDirection::None)
		{
			auto selected = nearestInDirection(resource.mLiftDirection);
			if (selected != ~0u) return selected;
			auto reverse = resource.mLiftDirection == TraversalDirection::Ascending
				? TraversalDirection::Descending : TraversalDirection::Ascending;
			selected = nearestInDirection(reverse);
			if (selected != ~0u)
			{
				resource.mLiftDirection = reverse;
				return selected;
			}
		}

		// Idle dispatch is based on the oldest individual interest. Actual distance
		// and stable stop ID resolve simultaneous calls deterministically.
		uint32_t selected = ~0u;
		uint64_t selectedTick = 0;
		float selectedDistance = 0.0f;
		for (uint32_t stop = 0; stop < resource.mLiftStops.size(); ++stop)
		{
			if (!requested(stop)) continue;
			auto oldest = min_element(resource.mLiftStopRequestTicks[stop].begin(),
				resource.mLiftStopRequestTicks[stop].end(), [](auto const& left, auto const& right)
				{ return left.second != right.second ? left.second < right.second : left.first < right.first; });
			if (oldest == resource.mLiftStopRequestTicks[stop].end()) continue;
			auto distance = abs(resource.mLiftStops[stop].globalPosition - position);
			if (selected == ~0u || oldest->second < selectedTick
				|| (oldest->second == selectedTick && (distance < selectedDistance
					|| (distance == selectedDistance && stop < selected))))
			{
				selected = stop;
				selectedTick = oldest->second;
				selectedDistance = distance;
			}
		}
		if (selected != ~0u)
		{
			auto delta = resource.mLiftStops[selected].globalPosition - position;
			resource.mLiftDirection = delta > 0.0f ? TraversalDirection::Ascending
				: delta < 0.0f ? TraversalDirection::Descending : TraversalDirection::None;
		}
		return selected;
	}

	bool SimulationCoordinator::isLiftBoardingDirectionCompatible(TraversalResource& resource,
		uint32_t originStop, uint32_t destinationStop)
	{
		if (originStop >= resource.mLiftStops.size() || destinationStop >= resource.mLiftStops.size()
			|| originStop == destinationStop) return false;
		auto desired = resource.mLiftStops[destinationStop].globalPosition
			> resource.mLiftStops[originStop].globalPosition
			? TraversalDirection::Ascending : TraversalDirection::Descending;
		if (resource.mLiftDirection == TraversalDirection::None
			|| resource.mLiftDirection == desired)
		{
			resource.mLiftDirection = desired;
			return true;
		}
		// Reverse at this stop only after LOOK has exhausted demand ahead.
		for (uint32_t stop = 0; stop < resource.mLiftStops.size(); ++stop)
		{
			if (stop == originStop || resource.mLiftStopRequestOwners[stop].empty()) continue;
			auto delta = resource.mLiftStops[stop].globalPosition - resource.mLiftPosition;
			if ((resource.mLiftDirection == TraversalDirection::Ascending && delta <= 0.0f)
				|| (resource.mLiftDirection == TraversalDirection::Descending && delta >= 0.0f)) continue;
			for (auto owner : resource.mLiftStopRequestOwners[stop])
			{
				if (resource.mLiftPassengerDestinations.contains(owner)) return false;
				auto intent = resource.mLiftTripIntents.find(owner);
				if (intent == resource.mLiftTripIntents.end()) return false;
				auto ownerDirection = resource.mLiftStops[intent->second.destinationStop].globalPosition
					> resource.mLiftStops[intent->second.originStop].globalPosition
					? TraversalDirection::Ascending : TraversalDirection::Descending;
				if (ownerDirection == resource.mLiftDirection) return false;
			}
		}
		resource.mLiftDirection = desired;
		return true;
	}

	void SimulationCoordinator::releaseLiftAdmission(TraversalRequestId requestId, TraversalResource& resource)
	{
		if (auto request = mBuilding.mTraversalRequests.find(requestId);
			request && (request->mSourceSector != resource.mLiftSector || resource.mOpenPlatformLift))
		{
			auto intent = resource.mLiftTripIntents.find(request->mOwner);
			if (intent != resource.mLiftTripIntents.end())
			{
				removeLiftStopRequest(resource, intent->second.originStop, request->mOwner);
				resource.mLiftTripIntents.erase(intent);
			}
		}
		resource.mAdmissionQueue.erase(remove(resource.mAdmissionQueue.begin(),
			resource.mAdmissionQueue.end(), requestId), resource.mAdmissionQueue.end());
		resource.mLiftConfirmationQueue.erase(remove(resource.mLiftConfirmationQueue.begin(),
			resource.mLiftConfirmationQueue.end(), requestId), resource.mLiftConfirmationQueue.end());
		if (resource.mLiftActiveConfirmation == requestId)
			resource.mLiftActiveConfirmation = resource.mLiftConfirmationQueue.empty()
				? TraversalRequestId{} : resource.mLiftConfirmationQueue.front();
		for (auto& reservation : resource.mAdmissionReservations)
			if (reservation == requestId) reservation = {};
		if (resource.mLiftAdmissionReservation == requestId) resource.mLiftAdmissionReservation = {};
		if (resource.mOpenPlatformLift)
		{
			resource.mOpenPlatformMissedBoarding.erase(requestId);
			resource.mOpenPlatformMissedPositions.erase(requestId);
			for (auto& lane : resource.mQueueLanes)
				lane.queue.erase(remove(lane.queue.begin(), lane.queue.end(), requestId), lane.queue.end());
			if (auto request = mBuilding.mTraversalRequests.find(requestId))
			{
				request->mQueuePosition = ~0u;
				request->mQueueApproach = ~0u;
				if (auto agent = mBuilding.mAgents.find(request->mOwner)) agent->mTraversalLocalGoal.reset();
			}
			mBuilding.refreshQueuePositions(resource);
		}
		if (auto request = mBuilding.mTraversalRequests.find(requestId)) request->mCapacityPosition = ~0u;
	}

	void SimulationCoordinator::requestLiftPassengerSafeExit(AgentId passenger, TraversalFailureReason reason)
	{
		for (auto const& [resourceId, resourcePtr] : mBuilding.mTraversalResources.entries())
		{
			(void)resourceId;
			auto& resource = *resourcePtr;
			if ((!resource.mLift && !resource.mShuttle)
				|| find(resource.mOccupants.begin(), resource.mOccupants.end(), passenger)
				== resource.mOccupants.end()) continue;
			for (uint32_t stop = 0; stop < resource.mLiftStopRequestOwners.size(); ++stop)
				removeLiftStopRequest(resource, stop, passenger);
			resource.mLiftPassengerDestinations.erase(passenger);
			resource.mLiftExitAtSafeStop.insert(passenger);
			auto failure = resource.mLiftExitFailures.find(passenger);
			if (failure == resource.mLiftExitFailures.end()
				|| failure->second == TraversalFailureReason::None)
				resource.mLiftExitFailures[passenger] = reason;
			auto safeStop = resource.mLiftMoving && resource.mLiftTargetStop < resource.mLiftStops.size()
				? resource.mLiftTargetStop : resource.mLiftCurrentStop;
			addLiftStopRequest(resource, safeStop, passenger);
			return;
		}
	}

	void SimulationCoordinator::assignLiftSafeExitPaths(TraversalResource& resource)
	{
		if (resource.mLiftMoving || resource.mLiftCurrentStop >= resource.mLiftStops.size()
			|| (resource.mLiftStopPhase != LiftStopPhase::Opening
				&& resource.mLiftStopPhase != LiftStopPhase::Disembarking
				&& resource.mLiftStopPhase != LiftStopPhase::Boarding)) return;
		vector<AgentId> assigned;
		vector<AgentId> gone;
		for (auto passenger : resource.mLiftExitAtSafeStop)
		{
			if (resource.mOpenPlatformLift)
			{
				for (auto& occupant : resource.mOccupants) if (occupant == passenger) occupant = {};
				for (uint32_t stop = 0; stop < resource.mLiftStopRequestOwners.size(); ++stop)
					removeLiftStopRequest(resource, stop, passenger);
				resource.mLiftPassengerDestinations.erase(passenger);
				resource.mLiftExitFailures.erase(passenger);
				if (auto agent = mBuilding.mAgents.find(passenger))
				{
					auto location = mBuilding.mSectors[(size_t)resource.mLiftSector.value - 1].get();
					auto global = agent->getGlobalPosition();
					global.y = resource.mLiftPosition;
					agent->setPosition({ location, global - location->getPosition() }, false);
					if (agent->getState() != Agent::State::Idle) agent->clearRuntimePath();
				}
				assigned.push_back(passenger);
				continue;
			}
			auto agent = mBuilding.mAgents.find(passenger);
			if (!agent || agent->getSector() != mBuilding.mSectors[(size_t)resource.mLiftSector.value - 1].get())
			{
				// The passenger is no longer here. Forgetting it from the pending-exit
				// set alone would leave the manifest slot standing forever (#57).
				gone.push_back(passenger);
				continue;
			}
			auto landingId = resource.mLiftStops[resource.mLiftCurrentStop].landingResource;
			if (resource.mShuttle)
			{
				auto carriage = findShuttlePassengerCarriage(resource, passenger);
				auto door = find_if(resource.mShuttleDoors.begin(), resource.mShuttleDoors.end(),
					[&](auto const& value) { return value.stopIndex == resource.mLiftCurrentStop
						&& value.carriageIndex == carriage; });
				if (door != resource.mShuttleDoors.end()) landingId = door->landingResource;
			}
			shared_ptr<const Edge> landingEdge;
			shared_ptr<const Vertex> source;
			shared_ptr<const Vertex> destination;
			for (auto const& edge : mBuilding.mGraph->getEdges())
			{
				if (edge->getTraversalResourceId() != landingId) continue;
				auto first = edge->getVertex(0);
				auto second = edge->getVertex(1);
				if (SectorId{ (uint64_t)first->getSector()->getIndex() + 1 } == resource.mLiftSector)
				{ source = first; destination = second; }
				else if (SectorId{ (uint64_t)second->getSector()->getIndex() + 1 } == resource.mLiftSector)
				{ source = second; destination = first; }
				if (source) { landingEdge = edge; break; }
			}
			if (!landingEdge) continue;
			auto path = make_shared<Path>();
			path->nodes.push_back({ nullptr, source, 0.0f });
			path->nodes.push_back({ landingEdge, destination, landingEdge->getWeight(destination, agent, true) });
			agent->assignPath(std::move(path), true, false);
			assigned.push_back(passenger);
		}
		for (auto passenger : assigned) resource.mLiftExitAtSafeStop.erase(passenger);
		// Released outside the loop: releaseAgentFromResource() also prunes the
		// pending-exit set, which this loop is still iterating.
		for (auto passenger : gone) releaseAgentFromResource(resource, passenger);
	}

	bool SimulationCoordinator::replaceOnboardLiftDestination(Agent& agent, shared_ptr<Path> const& path,
		uint32_t& sourceNode)
	{
		auto owner = getAgentId(&agent);
		if (!owner || !path) return false;
		for (auto const& [resourceId, resourcePtr] : mBuilding.mTraversalResources.entries())
		{
			(void)resourceId;
			auto& resource = *resourcePtr;
			if ((!resource.mLift && !resource.mShuttle) || !resource.mEnabled
				|| find(resource.mOccupants.begin(), resource.mOccupants.end(), owner) == resource.mOccupants.end()) continue;
			for (uint32_t i = 0; i + 1 < path->nodes.size(); ++i)
			{
				auto const& node = path->nodes[i + 1];
				if (!node.edge || (node.edge->getType() != EdgeType::Lift
					&& node.edge->getType() != EdgeType::Shuttle) || !node.targetVertex) continue;
				auto destinationStop = findLiftStop(resource, node.targetVertex->getPosition());
				if (destinationStop >= resource.mLiftStops.size()) return false;
				for (uint32_t stop = 0; stop < resource.mLiftStopRequestOwners.size(); ++stop)
					removeLiftStopRequest(resource, stop, owner);
				resource.mLiftPassengerDestinations.erase(owner);
				// Allocation below will either share an already-active destination or
				// serialize a fresh selection at the interior control.
				resource.mLiftExitAtSafeStop.erase(owner);
				resource.mLiftExitFailures.erase(owner);
				vector<InteractionRequestId> obsoleteSelections;
				for (auto const& [interactionId, interaction] : mBuilding.mInteractionRequests.entries())
					if (interaction->mActor == owner && interaction->mResult == InteractionResult::Pending)
						obsoleteSelections.push_back(interactionId);
				for (auto interactionId : obsoleteSelections) cancelInteraction(interactionId);
				if (agent.mTraversalTask)
				{
					if (auto request = mBuilding.mTraversalRequests.find(agent.mTraversalTask->request))
					{
						request->mSourceEndpoint = path->nodes[i].targetVertex->getPosition();
						request->mDestinationEndpoint = node.targetVertex->getPosition();
						request->mPreparationRequested = false;
						request->mPreparationOperation = {};
						request->mPreparationAttempts = 0;
						request->mNextPreparationTick = mBuilding.mSimulationTick;
					}
				}
				sourceNode = i;
				return true;
			}
		}
		return false;
	}

	// Riding allocation. The Agent is already an occupant of the car and asks to
	// travel to another stop inside it. A destination already scheduled for the
	// passenger is granted straight away - for a shuttle, after the passenger has
	// walked within the carriage to the final node of the contiguous ride so the
	// whole chain commits as one journey. Otherwise the journey stop is read from
	// the ride edge ahead on the path: a stop some other passenger has already
	// requested is shared without further ceremony, and a fresh stop is confirmed
	// one passenger at a time through the confirmation queue at the interior
	// selector control. A failed confirmation is retried on the waiting policy's
	// delay until the retries run out, at which point the passenger is asked to
	// leave at the next safe stop and the request is denied.
	void SimulationCoordinator::allocateLiftRiding(TraversalRequestId requestId, TraversalResource& coordinator)
	{
		auto request = mBuilding.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		if (find(coordinator.mOccupants.begin(), coordinator.mOccupants.end(), request->mOwner)
			== coordinator.mOccupants.end()) return;
		if (auto scheduled = coordinator.mLiftPassengerDestinations.find(request->mOwner);
			scheduled != coordinator.mLiftPassengerDestinations.end())
		{
			if (!coordinator.mLiftMoving && coordinator.mLiftCurrentStop == scheduled->second)
			{
				if (auto actor = mBuilding.mAgents.find(request->mOwner))
				{
					if (coordinator.mShuttle)
					{
						// Multiple Door cells create a contiguous chain of Shuttle
						// edges. Intermediate nodes may still belong to the origin
						// stop, so align with the final node in this journey rather
						// than the current edge's endpoint.
						auto destinationVertex = actor->mTraversalTask
							? actor->mTraversalTask->destinationVertex : shared_ptr<const Vertex>{};
						auto destinationNode = actor->mPath.targetNode + 1;
						if (actor->mPath.path)
							for (uint32_t i = actor->mPath.targetNode + 1;
								i < actor->mPath.path->nodes.size(); ++i)
							{
								auto const& node = actor->mPath.path->nodes[i];
								if (!node.edge || node.edge->getType() != EdgeType::Shuttle
									|| node.edge->getTraversalResourceId()
										!= coordinator.mShuttle->getTraversalResourceId()) break;
								if (node.targetVertex)
								{
									destinationVertex = node.targetVertex;
									destinationNode = i;
								}
							}
						if (!destinationVertex) return;

						auto destinationEndpoint = destinationVertex->getPosition();
						auto alignmentTarget = actor->getGlobalPosition();
						alignmentTarget.x = destinationEndpoint.x;
						if (abs(actor->getGlobalPosition().x - alignmentTarget.x) > 0.001f)
						{
							actor->mTraversalLocalGoal = alignmentTarget;
							return;
						}
						actor->mTraversalLocalGoal.reset();

						// Commit the contiguous ride as one journey so Agent does not
						// subsequently traverse stale intermediate Shuttle nodes.
						request->mDestinationEndpoint = destinationEndpoint;
						request->mDestinationSector = SectorId{
							(uint64_t)destinationVertex->getSector()->getIndex() + 1 };
						if (actor->mTraversalTask)
							actor->mTraversalTask->destinationVertex = destinationVertex;
						if (destinationNode > actor->mPath.targetNode)
							actor->mPath.targetNode = destinationNode - 1;
					}
					else
					{
						auto transit = mBuilding.mSectors[(size_t)coordinator.mLiftSector.value - 1].get();
						actor->setPosition({ transit,
							request->mDestinationEndpoint - transit->getPosition() }, false);
					}
				}
				mBuilding.grantTraversalRequest(requestId);
			}
			return;
		}
		auto actor = mBuilding.mAgents.find(request->mOwner);
		auto journeyStop = actor ? findAgentLiftDestination(*actor, coordinator) : ~0u;
		if (journeyStop >= coordinator.mLiftStops.size()) { mBuilding.denyTraversalRequest(requestId); return; }
		if (!coordinator.mLiftStopRequestOwners[journeyStop].empty())
		{
			addLiftStopRequest(coordinator, journeyStop, request->mOwner);
			coordinator.mLiftPassengerDestinations[request->mOwner] = journeyStop;
			// This passenger may have queued for serialized destination
			// confirmation before another passenger activated the same stop.
			// Sharing that destination makes the queued confirmation obsolete.
			coordinator.mLiftConfirmationQueue.erase(remove(
				coordinator.mLiftConfirmationQueue.begin(),
				coordinator.mLiftConfirmationQueue.end(), requestId),
				coordinator.mLiftConfirmationQueue.end());
			if (coordinator.mLiftActiveConfirmation == requestId)
				coordinator.mLiftActiveConfirmation = coordinator.mLiftConfirmationQueue.empty()
					? TraversalRequestId{} : coordinator.mLiftConfirmationQueue.front();
			return;
		}
		if (find(coordinator.mLiftConfirmationQueue.begin(), coordinator.mLiftConfirmationQueue.end(), requestId)
			== coordinator.mLiftConfirmationQueue.end())
			coordinator.mLiftConfirmationQueue.push_back(requestId);
		if (!coordinator.mLiftActiveConfirmation)
			coordinator.mLiftActiveConfirmation = coordinator.mLiftConfirmationQueue.front();
		if (coordinator.mLiftActiveConfirmation != requestId) return;
		if (!request->mPreparationRequested)
		{
			if (mBuilding.mSimulationTick < request->mNextPreparationTick) return;
			if (journeyStop >= coordinator.mControls.size()) { mBuilding.denyTraversalRequest(requestId); return; }
			coordinator.mLiftSelector = coordinator.mControls[journeyStop];
			auto selector = mBuilding.mInteractionPoints.find(coordinator.mLiftSelector);
			auto actor = mBuilding.mAgents.find(request->mOwner);
			if (selector && actor) selector->mPosition = actor->getGlobalPosition();
			auto interactionId = requestInteractionForTraversal(coordinator.mLiftSelector, request->mOwner);
			if (!interactionId) return;
			auto interaction = mBuilding.mInteractionRequests.find(interactionId);
			request->mPreparationRequested = true;
			if (interaction && !interaction->mOperations.empty())
				request->mPreparationOperation = interaction->mOperations.front().first;
			return;
		}
		auto operation = mBuilding.mDeviceOperations.find(request->mPreparationOperation);
		if (!operation || operation->mState == DeviceOperationState::Pending
			|| operation->mState == DeviceOperationState::Running) return;
		if (operation->mState != DeviceOperationState::Succeeded)
		{
			if (request->mPreparationAttempts < mBuilding.mTraversalWaitingPolicy.maximumDestinationRetries)
			{
				++request->mPreparationAttempts;
				for (auto const& [interactionId, interaction] : mBuilding.mInteractionRequests.entries())
					if (interaction->mActor == request->mOwner
						&& interaction->mResult == InteractionResult::Pending)
						cancelInteraction(interactionId);
				request->mPreparationRequested = false;
				request->mPreparationOperation = {};
				request->mNextPreparationTick = mBuilding.mSimulationTick
					+ mBuilding.mTraversalWaitingPolicy.destinationRetryDelayTicks;
				return;
			}
			requestLiftPassengerSafeExit(request->mOwner, TraversalFailureReason::PreparationFailed);
			mBuilding.denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
			return;
		}
		addLiftStopRequest(coordinator, journeyStop, request->mOwner);
		coordinator.mLiftPassengerDestinations[request->mOwner] = journeyStop;
		coordinator.mLiftDestinationStop = journeyStop;
		coordinator.mLiftConfirmationQueue.erase(coordinator.mLiftConfirmationQueue.begin());
		coordinator.mLiftActiveConfirmation = coordinator.mLiftConfirmationQueue.empty()
			? TraversalRequestId{} : coordinator.mLiftConfirmationQueue.front();
		return;
	}

	// Disembarking allocation. The Agent occupies the car and asks to leave it at
	// the stop the car is standing at. A shuttle passenger is first assigned the
	// disembark door which serves its assigned carriage, then walks within the
	// carriage to the shuttle-side node the remaining path selected before the
	// Door crossing is granted; a lift passenger crosses the landing it asked to
	// leave through. The stop enters its Disembarking phase, a door open lease
	// holds the landing door open for the crossing, and the grant takes the first
	// free crossing lane on the landing resource.
	void SimulationCoordinator::allocateLiftDisembarking(TraversalRequestId requestId,
		TraversalResource& edgeResource, TraversalResource& coordinator, uint32_t stop)
	{
		auto request = mBuilding.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		if (find(coordinator.mOccupants.begin(), coordinator.mOccupants.end(), request->mOwner)
			== coordinator.mOccupants.end()
			|| coordinator.mLiftMoving || coordinator.mLiftCurrentStop != stop) return;
		auto disembarkLanding = &edgeResource;
		if (coordinator.mShuttle)
		{
			if (!assignShuttleDisembarkDoor(requestId, coordinator, stop)) return;
			disembarkLanding = mBuilding.mTraversalResources.find(request->mResource);
			if (!disembarkLanding) return;

			// Once the Shuttle has stopped, walk within the carriage to the
			// shuttle-side node selected by the remaining path before granting the
			// Door crossing. Preserve the passenger's standing Y coordinate.
			auto actor = mBuilding.mAgents.find(request->mOwner);
			if (!actor) return;
			auto alignmentTarget = actor->getGlobalPosition();
			alignmentTarget.x = request->mSourceEndpoint.x;
			if (abs(actor->getGlobalPosition().x - alignmentTarget.x) > 0.001f)
			{
				actor->mTraversalLocalGoal = alignmentTarget;
				return;
			}
			actor->mTraversalLocalGoal.reset();
		}
		coordinator.mLiftStopPhase = LiftStopPhase::Disembarking;
		if (!request->mPreparationLease)
			request->mPreparationLease = acquireDoorOpenLease(*disembarkLanding,
				DoorOpenLeaseKind::Preparation, requestId);
		if (!disembarkLanding->mDoor->isOpen())
		{
			if (!disembarkLanding->mDoor->isOpening()) disembarkLanding->mDoor->requestOpen();
			return;
		}
		auto lane = find(disembarkLanding->mCrossingOwners.begin(), disembarkLanding->mCrossingOwners.end(), TraversalRequestId{});
		if (lane == disembarkLanding->mCrossingOwners.end()) return;
		request->mCrossingLane = (uint32_t)distance(disembarkLanding->mCrossingOwners.begin(), lane);
		*lane = requestId;
		coordinator.mLiftCarDoorOpen = true;
		mBuilding.grantTraversalRequest(requestId);
	}

} // core
