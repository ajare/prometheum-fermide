#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

#include "core/SimulationCoordinator.h"

#include "core/Agent.h"
#include "core/Building.h"
#include "core/Button.h"
#include "core/Coordination.h"
#include "core/ExtensibleObject.h"
#include "core/Sector.h"
#include "core/Simulation.h"


namespace core
{

	using namespace std;

	// Interaction and device-operation orchestration moved out of Building
	// (ADR 0004 stage 2). The behaviour is unchanged: the coordinator works on
	// Building's interaction and device-operation registries through friendship,
	// and calls back through the Building facade for the machinery which has not
	// moved out of Building yet - snapshots and the structural-edit guard. The
	// lift stop request it makes for an accepted lift call goes to the
	// coordinator's own lift scheduling helper.

	InteractionPointId SimulationCoordinator::createInteractionPoint(string const& name)
	{
		auto id = mBuilding.mInteractionPoints.add(unique_ptr<InteractionPoint>(new InteractionPoint(name)));
		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::InteractionPointAdded;
		event.interactionPoint = mBuilding.makeInteractionPointSnapshot(id, *mBuilding.mInteractionPoints.find(id));
		mBuilding.mEvents.push_back(std::move(event));
		return id;
	}

	InteractionPointId SimulationCoordinator::createInteractionPoint(string const& name, SectorId sector,
		Vector2 position, float reach, float durationSeconds, vector<InteractionBinding> bindings)
	{
		if (!sector || sector.value > mBuilding.mSectors.size())
		{
			throw invalid_argument("An interaction point requires a valid sector");
		}
		if (reach < 0.0f || durationSeconds < 0.0f || bindings.empty())
		{
			throw invalid_argument("An interaction point requires non-negative timing and at least one binding");
		}
		for (auto const& binding : bindings)
		{
			bool validTarget = false;
			if (binding.command.type == DeviceCommandType::SetSectorLights)
				validTarget = binding.command.target && binding.command.target.value <= mBuilding.mSectors.size();
			else if (auto resource = mBuilding.mTraversalResources.find(binding.command.traversalResource))
				validTarget = binding.command.type == DeviceCommandType::OpenDoor ? resource->mDoor != nullptr
					: binding.command.type == DeviceCommandType::SetExtendedState ? resource->mExtensible != nullptr
					: (binding.command.type == DeviceCommandType::CallLift
						|| binding.command.type == DeviceCommandType::SelectLiftDestination
						|| binding.command.type == DeviceCommandType::CallShuttle
						|| binding.command.type == DeviceCommandType::SelectShuttleDestination)
						&& (resource->mLift || resource->mShuttle)
						&& binding.command.stopIndex < resource->mLiftStops.size();
			if (!validTarget)
			{
				throw invalid_argument("An interaction binding requires a valid command target");
			}
		}
		auto durationTicks = max<uint64_t>(1, (uint64_t)ceil(durationSeconds / Building::getFixedTimestep()));
		auto id = mBuilding.mInteractionPoints.add(unique_ptr<InteractionPoint>(new InteractionPoint(
			name, sector, position, reach, durationTicks, std::move(bindings))));
		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::InteractionPointAdded;
		event.interactionPoint = mBuilding.makeInteractionPointSnapshot(id, *mBuilding.mInteractionPoints.find(id));
		mBuilding.mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<InteractionPoint> SimulationCoordinator::lookupInteractionPoint(InteractionPointId id)
	{
		auto entity = mBuilding.mInteractionPoints.find(id);
		return entity ? EntityLookup<InteractionPoint>{ entity, {} }
			: EntityLookup<InteractionPoint>{ nullptr, format("InteractionPoint handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<InteractionPoint const> SimulationCoordinator::lookupInteractionPoint(InteractionPointId id) const
	{
		auto entity = mBuilding.mInteractionPoints.find(id);
		return entity ? EntityLookup<InteractionPoint const>{ entity, {} }
			: EntityLookup<InteractionPoint const>{ nullptr, format("InteractionPoint handle {} is invalid or has been removed", id.value) };
	}

	EntityRemovalResult SimulationCoordinator::removeInteractionPoint(InteractionPointId id)
	{
		auto found = lookupInteractionPoint(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		bool structural = any_of(mBuilding.mTraversalResources.entries().begin(), mBuilding.mTraversalResources.entries().end(),
			[id](auto const& entry)
			{
				auto const& resource = *entry.second;
				if (find(resource.mControls.begin(), resource.mControls.end(), id) != resource.mControls.end()) return true;
				if (resource.mLiftSelector == id) return true;
				return any_of(resource.mLiftStops.begin(), resource.mLiftStops.end(),
					[id](auto const& stop) { return stop.callControl == id; });
			});
		if (structural) mBuilding.beginStructuralEdit("removeInteractionPoint");
		vector<InteractionRequestId> requests;
		for (auto const& [requestId, request] : mBuilding.mInteractionRequests.entries())
		{
			if (request->getPoint() == id && request->getResult() == InteractionResult::Pending)
			{
				requests.push_back(requestId);
			}
		}
		for (auto requestId : requests)
		{
			cancelInteraction(requestId);
		}
		auto snapshot = mBuilding.makeInteractionPointSnapshot(id, *found.entity);
		mBuilding.mInteractionPoints.remove(id);

		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::InteractionPointRemoved;
		event.interactionPoint = std::move(snapshot);
		mBuilding.mEvents.push_back(std::move(event));
		return { true, {} };
	}

	DeviceOperationId SimulationCoordinator::findOrCreateDeviceOperation(DeviceCommand const& command, AgentId requester)
	{
		for (auto const& [id, operation] : mBuilding.mDeviceOperations.entries())
		{
			if (operation->mHasCommand && operation->mCommand == command
				&& (operation->mState == DeviceOperationState::Pending || operation->mState == DeviceOperationState::Running))
			{
				operation->mRequesters.insert(requester);
				return id;
			}
		}
		auto name = command.type == DeviceCommandType::SetSectorLights
			? string("Set sector lights ") + (command.desiredState ? "on" : "off")
			: command.type == DeviceCommandType::OpenDoor ? "Open door"
			: command.type == DeviceCommandType::SetExtendedState
				? string(command.desiredState ? "Extend resource" : "Retract resource")
			: command.type == DeviceCommandType::CallLift ? "Call lift"
			: command.type == DeviceCommandType::SelectLiftDestination ? "Select lift destination"
			: command.type == DeviceCommandType::CallShuttle ? "Call shuttle"
			: command.type == DeviceCommandType::SelectShuttleDestination ? "Select shuttle destination"
				: "Device command";
		auto id = mBuilding.mDeviceOperations.add(unique_ptr<DeviceOperation>(new DeviceOperation(name, requester, command)));
		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::DeviceOperationAdded;
		event.phase = mBuilding.mCurrentPhase;
		event.deviceOperation = mBuilding.makeDeviceOperationSnapshot(id, *mBuilding.mDeviceOperations.find(id));
		mBuilding.mEvents.push_back(std::move(event));
		return id;
	}

	InteractionRequestId SimulationCoordinator::requestInteraction(InteractionPointId pointId, AgentId actorId)
	{
		auto point = mBuilding.mInteractionPoints.find(pointId);
		auto actor = mBuilding.mAgents.find(actorId);
		if (!point || !actor || !point->mSector
			|| (actor->getState() != Agent::State::Idle
				&& actor->getState() != Agent::State::WaitingForTraversal)
			|| actor->getSector() != mBuilding.mSectors[(size_t)point->mSector.value - 1].get())
		{
			return {};
		}
		for (auto const& [id, request] : mBuilding.mInteractionRequests.entries())
		{
			if (request->mActor == actorId && request->mResult == InteractionResult::Pending)
			{
				return request->mPoint == pointId ? id : InteractionRequestId{};
			}
		}
		auto id = mBuilding.mInteractionRequests.add(unique_ptr<InteractionRequest>(new InteractionRequest(pointId, actorId)));
		auto request = mBuilding.mInteractionRequests.find(id);
		for (auto const& binding : point->mBindings)
		{
			request->mOperations.emplace_back(findOrCreateDeviceOperation(binding.command, actorId), binding.requirement);
		}
		point->mQueue.push_back(id);

		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::InteractionRequestAdded;
		event.phase = mBuilding.mCurrentPhase;
		event.interactionRequest = mBuilding.makeInteractionRequestSnapshot(id, *request);
		mBuilding.mEvents.push_back(std::move(event));
		return id;
	}

	InteractionRequestId SimulationCoordinator::requestInteractionForTraversal(InteractionPointId point, AgentId actor)
	{
		return requestInteraction(point, actor);
	}

	InteractionRequestId SimulationCoordinator::requestInteractionWhilePassing(
		InteractionPointId pointId, AgentId actorId)
	{
		auto point = mBuilding.mInteractionPoints.find(pointId);
		auto actor = mBuilding.mAgents.find(actorId);
		if (!point || !actor
			|| actor->getSector() != mBuilding.mSectors[(size_t)point->mSector.value - 1].get())
		{
			return {};
		}
		for (auto const& [id, request] : mBuilding.mInteractionRequests.entries())
		{
			(void)id;
			if (request->mActor == actorId && request->mResult == InteractionResult::Pending)
				return {};
		}

		auto id = mBuilding.mInteractionRequests.add(unique_ptr<InteractionRequest>(
			new InteractionRequest(pointId, actorId)));
		auto request = mBuilding.mInteractionRequests.find(id);
		for (auto const& binding : point->mBindings)
		{
			auto operationId = findOrCreateDeviceOperation(binding.command, actorId);
			request->mOperations.emplace_back(operationId, binding.requirement);
			if (auto operation = mBuilding.mDeviceOperations.find(operationId)) operation->mActivated = true;
		}
		pressPhysicalControl(pointId);

		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::InteractionRequestAdded;
		event.phase = mBuilding.mCurrentPhase;
		event.interactionRequest = mBuilding.makeInteractionRequestSnapshot(id, *request);
		mBuilding.mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<InteractionRequest const> SimulationCoordinator::lookupInteractionRequest(InteractionRequestId id) const
	{
		auto entity = mBuilding.mInteractionRequests.find(id);
		return entity ? EntityLookup<InteractionRequest const>{ entity, {} }
			: EntityLookup<InteractionRequest const>{ nullptr, format("InteractionRequest handle {} is invalid", id.value) };
	}

	void SimulationCoordinator::detachInteractionRequester(InteractionRequest& request)
	{
		for (auto const& [operationId, requirement] : request.mOperations)
		{
			(void)requirement;
			if (auto operation = mBuilding.mDeviceOperations.find(operationId))
			{
				operation->mRequesters.erase(request.mActor);
				if (operation->mRequesters.empty() && (operation->mState == DeviceOperationState::Pending
					|| operation->mState == DeviceOperationState::Running))
				{
					operation->mState = DeviceOperationState::Cancelled;
				}
			}
		}
	}

	bool SimulationCoordinator::cancelInteraction(InteractionRequestId id)
	{
		auto request = mBuilding.mInteractionRequests.find(id);
		if (!request || request->mResult != InteractionResult::Pending)
		{
			return false;
		}
		request->mResult = InteractionResult::Cancelled;
		detachInteractionRequester(*request);
		if (auto point = mBuilding.mInteractionPoints.find(request->mPoint))
		{
			if (point->mActiveRequest == id)
			{
				point->mActiveRequest = {};
				point->mInteractionTicksRemaining = 0;
			}
			point->mQueue.erase(remove(point->mQueue.begin(), point->mQueue.end(), id), point->mQueue.end());
		}
		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::InteractionRequestChanged;
		event.phase = mBuilding.mCurrentPhase;
		event.interactionRequest = mBuilding.makeInteractionRequestSnapshot(id, *request);
		mBuilding.mEvents.push_back(std::move(event));
		return true;
	}

	DeviceOperationId SimulationCoordinator::createDeviceOperation(string const& name, AgentId requester)
	{
		auto agent = lookupAgent(requester);
		if (!agent)
		{
			throw invalid_argument(format("Cannot create DeviceOperation: {}", agent.diagnostic));
		}

		auto id = mBuilding.mDeviceOperations.add(unique_ptr<DeviceOperation>(new DeviceOperation(name, requester)));
		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::DeviceOperationAdded;
		event.deviceOperation = mBuilding.makeDeviceOperationSnapshot(id, *mBuilding.mDeviceOperations.find(id));
		mBuilding.mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<DeviceOperation> SimulationCoordinator::lookupDeviceOperation(DeviceOperationId id)
	{
		auto entity = mBuilding.mDeviceOperations.find(id);
		return entity ? EntityLookup<DeviceOperation>{ entity, {} }
			: EntityLookup<DeviceOperation>{ nullptr, format("DeviceOperation handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<DeviceOperation const> SimulationCoordinator::lookupDeviceOperation(DeviceOperationId id) const
	{
		auto entity = mBuilding.mDeviceOperations.find(id);
		return entity ? EntityLookup<DeviceOperation const>{ entity, {} }
			: EntityLookup<DeviceOperation const>{ nullptr, format("DeviceOperation handle {} is invalid or has been removed", id.value) };
	}

	bool SimulationCoordinator::cancelDeviceOperation(DeviceOperationId id, AgentId requester)
	{
		auto operation = mBuilding.mDeviceOperations.find(id);
		if (!operation || !operation->mRequesters.erase(requester))
		{
			return false;
		}
		vector<InteractionRequestId> affectedRequests;
		for (auto const& [requestId, request] : mBuilding.mInteractionRequests.entries())
		{
			if (request->mActor != requester || request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			if (find_if(request->mOperations.begin(), request->mOperations.end(), [id](auto const& binding)
				{ return binding.first == id; }) != request->mOperations.end())
			{
				affectedRequests.push_back(requestId);
			}
		}
		for (auto requestId : affectedRequests)
		{
			cancelInteraction(requestId);
		}
		if (operation->mRequesters.empty() && (operation->mState == DeviceOperationState::Pending
			|| operation->mState == DeviceOperationState::Running))
		{
			operation->mState = DeviceOperationState::Cancelled;
		}
		return true;
	}

	EntityRemovalResult SimulationCoordinator::removeDeviceOperation(DeviceOperationId id)
	{
		auto found = lookupDeviceOperation(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		auto snapshot = mBuilding.makeDeviceOperationSnapshot(id, *found.entity);
		mBuilding.mDeviceOperations.remove(id);

		SimulationEvent event;
		event.sequence = mBuilding.mNextEventSequence++;
		event.tick = mBuilding.mSimulationTick;
		event.type = SimulationEventType::DeviceOperationRemoved;
		event.deviceOperation = std::move(snapshot);
		mBuilding.mEvents.push_back(std::move(event));
		return { true, {} };
	}

	void SimulationCoordinator::advanceDeviceOperations()
	{
		for (auto const& [id, operation] : mBuilding.mDeviceOperations.entries())
		{
			(void)id;
			if (!operation->mHasCommand || !operation->mActivated)
			{
				continue;
			}
			if (operation->mState == DeviceOperationState::Pending)
			{
				operation->mState = DeviceOperationState::Running;
				if (operation->mCommand.type == DeviceCommandType::OpenDoor)
				{
					auto resource = mBuilding.mTraversalResources.find(operation->mCommand.traversalResource);
					if (!resource || !resource->mDoor || !resource->mEnabled
						|| !(operation->mCommand.desiredState
							? resource->mDoor->requestOpen() : resource->mDoor->requestClose()))
					{
						operation->mState = DeviceOperationState::Rejected;
					}
				}
				else if (operation->mCommand.type == DeviceCommandType::SetExtendedState)
				{
					auto resource = mBuilding.mTraversalResources.find(operation->mCommand.traversalResource);
					if (!resource || !resource->mExtensible || !resource->mExtensible->isExtensible())
					{
						operation->mState = DeviceOperationState::Rejected;
					}
					else if (operation->mCommand.desiredState)
					{
						resource->mRetractionPending = false;
						resource->mEnabled = true;
						if (!resource->mExtensible->extend())
							operation->mState = DeviceOperationState::Rejected;
					}
					else
					{
						// Accepting a safe retract closes admission immediately. Physical
						// retraction starts only after every independently-owned lease drains.
						resource->mRetractionPending = true;
						resource->mEnabled = false;
					}
				}
				continue;
			}
			if (operation->mState != DeviceOperationState::Running)
			{
				continue;
			}
			if (operation->mCommand.type == DeviceCommandType::SetSectorLights
				&& operation->mCommand.target
				&& operation->mCommand.target.value <= mBuilding.mSectors.size())
			{
				auto sector = mBuilding.mSectors[(size_t)operation->mCommand.target.value - 1];
				bool succeeded = operation->mCommand.desiredState ? sector->lightsOn() : sector->lightsOff();
				operation->mState = succeeded ? DeviceOperationState::Succeeded : DeviceOperationState::Failed;
			}
			else if (operation->mCommand.type == DeviceCommandType::SetExtendedState)
			{
				auto resource = mBuilding.mTraversalResources.find(operation->mCommand.traversalResource);
				if (!resource || !resource->mExtensible)
				{
					operation->mState = DeviceOperationState::Failed;
				}
				else if (operation->mCommand.desiredState && resource->mExtensible->isExtended())
				{
					operation->mState = DeviceOperationState::Succeeded;
				}
				else if (!operation->mCommand.desiredState)
				{
					if (resource->mExtensionRequestLeases.empty()
						&& resource->mExtensionOccupantLeases.empty())
					{
						if (!resource->mExtensible->isRetracted() && !resource->mExtensible->isRetracting())
							resource->mExtensible->retract();
						if (resource->mExtensible->isRetracted())
						{
							resource->mRetractionPending = false;
							operation->mState = DeviceOperationState::Succeeded;
						}
					}
				}
			}
			else if (operation->mCommand.type == DeviceCommandType::CallLift
				|| operation->mCommand.type == DeviceCommandType::SelectLiftDestination
				|| operation->mCommand.type == DeviceCommandType::CallShuttle
				|| operation->mCommand.type == DeviceCommandType::SelectShuttleDestination)
			{
				auto resource = mBuilding.mTraversalResources.find(operation->mCommand.traversalResource);
				if (!resource || (!resource->mLift && !resource->mShuttle) || !resource->mEnabled
					|| operation->mCommand.stopIndex >= resource->mLiftStops.size())
				{
					operation->mState = DeviceOperationState::Rejected;
				}
				else
				{
					if (operation->mCommand.type == DeviceCommandType::CallLift
						|| operation->mCommand.type == DeviceCommandType::CallShuttle)
					{
						// The operation may be shared by several waiting passengers. Each
						// keeps independent ownership even though the physical call coalesces.
						for (auto requester : operation->mRequesters)
							addLiftStopRequest(*resource, operation->mCommand.stopIndex, requester);
						if (resource->mLiftStopPhase == LiftStopPhase::Idle)
							resource->mLiftStopPhase = LiftStopPhase::Closing;
					}
					else resource->mLiftDestinationStop = operation->mCommand.stopIndex;
					operation->mState = DeviceOperationState::Succeeded;
				}
			}
			else if (operation->mCommand.type == DeviceCommandType::OpenDoor)
			{
				auto resource = mBuilding.mTraversalResources.find(operation->mCommand.traversalResource);
				if (!resource || !resource->mDoor)
				{
					operation->mState = DeviceOperationState::Failed;
				}
				else if (!resource->mEnabled)
				{
					operation->mState = DeviceOperationState::Rejected;
				}
				else if ((operation->mCommand.desiredState && resource->mDoor->isOpen())
					|| (!operation->mCommand.desiredState && resource->mDoor->isClosed()))
				{
					operation->mState = DeviceOperationState::Succeeded;
				}
			}
			else
			{
				operation->mState = DeviceOperationState::Failed;
			}
		}
	}

	void SimulationCoordinator::pressPhysicalControl(InteractionPointId pointId)
	{
		for (auto const& sector : mBuilding.mSectors)
		{
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				auto button = object
					? dynamic_pointer_cast<Button>(object->_getObject()) : nullptr;
				if (button && button->getInteractionPointId() == pointId)
				{
					button->disable();
					return;
				}
			}
		}
	}

	void SimulationCoordinator::tryPressUpcomingDoorButton(Agent& agent,
		Vector2 const& movementStart, Vector2 const& movementEnd)
	{
		auto const& path = agent.mPath.path;
		auto const target = agent.mPath.targetNode;
		if (!path || target + 1 >= path->nodes.size())
		{
			agent.mEarlyDoorPressResource = {};
			agent.mEarlyDoorPressAttempted = false;
			return;
		}

		// A Door Button contributes an Interactable vertex to the in-sector route.
		// Follow that route through its in-sector edges to find the next Door
		// threshold without looking beyond the current Sector.
		TraversalResourceId resourceId;
		auto currentSector = agent.getSector();
		for (uint32_t i = target + 1; i < path->nodes.size(); ++i)
		{
			auto const& node = path->nodes[i];
			if (!node.edge || !node.targetVertex) break;
			if (node.edge->getType() == EdgeType::Door)
			{
				auto sourceVertex = path->nodes[i - 1].targetVertex;
				if (sourceVertex && sourceVertex->getSector().get() == currentSector)
					resourceId = node.edge->getTraversalResourceId();
				break;
			}
			if (node.targetVertex->getSector().get() != currentSector) break;
		}
		auto resource = mBuilding.mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor
			|| resource->mDoorActivationMode != DoorActivationMode::RemoteControlled)
		{
			agent.mEarlyDoorPressResource = {};
			agent.mEarlyDoorPressAttempted = false;
			return;
		}
		if (agent.mEarlyDoorPressResource != resourceId)
		{
			agent.mEarlyDoorPressResource = resourceId;
			agent.mEarlyDoorPressAttempted = false;
		}
		if (agent.mEarlyDoorPressAttempted || !resource->mEnabled
			|| resource->mDoor->isOpen() || resource->mDoor->isOpening())
		{
			return;
		}

		auto actorId = getAgentId(&agent);
		for (auto const& [id, interaction] : mBuilding.mInteractionRequests.entries())
		{
			(void)id;
			if (interaction->mActor == actorId
				&& interaction->mResult == InteractionResult::Pending) return;
		}

		auto distanceToSegment = [&](Vector2 const& point)
		{
			auto delta = movementEnd - movementStart;
			auto lengthSquared = delta.x * delta.x + delta.y * delta.y;
			float amount = 0.0f;
			if (lengthSquared > 0.0f)
			{
				auto fromStart = point - movementStart;
				amount = clamp((fromStart.x * delta.x + fromStart.y * delta.y)
					/ lengthSquared, 0.0f, 1.0f);
			}
			return point.distanceTo(movementStart + delta * amount);
		};

		InteractionPointId selected;
		float selectedDistance = numeric_limits<float>::max();
		auto sourceSector = SectorId{ (uint64_t)agent.getSector()->getIndex() + 1 };
		for (auto pointId : resource->mControls)
		{
			auto point = mBuilding.mInteractionPoints.find(pointId);
			if (!point || point->mSector != sourceSector) continue;
			bool opensDoor = any_of(point->mBindings.begin(), point->mBindings.end(),
				[&](InteractionBinding const& binding)
				{
					return binding.command.type == DeviceCommandType::OpenDoor
						&& binding.command.desiredState
						&& binding.command.traversalResource == resourceId;
				});
			if (!opensDoor) continue;
			auto distance = distanceToSegment(point->mPosition);
			if (distance > point->mReach) continue;
			if (!selected || distance < selectedDistance - 0.001f
				|| (abs(distance - selectedDistance) <= 0.001f && pointId < selected))
			{
				selected = pointId;
				selectedDistance = distance;
			}
		}
		if (!selected) return;

		if (requestInteractionWhilePassing(selected, actorId))
			agent.mEarlyDoorPressAttempted = true;
	}

	void SimulationCoordinator::allocateInteractions()
	{
		for (auto const& [pointId, point] : mBuilding.mInteractionPoints.entries())
		{
			(void)pointId;
			if (point->mActiveRequest)
			{
				auto active = mBuilding.mInteractionRequests.find(point->mActiveRequest);
				if (active && active->mResult == InteractionResult::Pending)
				{
					continue;
				}
				point->mActiveRequest = {};
				point->mInteractionTicksRemaining = 0;
			}
			while (!point->mQueue.empty())
			{
				auto requestId = point->mQueue.front();
				auto request = mBuilding.mInteractionRequests.find(requestId);
				if (!request || request->mResult != InteractionResult::Pending)
				{
					point->mQueue.erase(point->mQueue.begin());
					continue;
				}
				bool reusedActiveWork = false;
				for (auto const& [operationId, requirement] : request->mOperations)
				{
					(void)requirement;
					if (auto operation = mBuilding.mDeviceOperations.find(operationId); operation && operation->mActivated)
					{
						reusedActiveWork = true;
						break;
					}
				}
				if (reusedActiveWork)
				{
					point->mQueue.erase(point->mQueue.begin());
					continue;
				}
				point->mActiveRequest = requestId;
				point->mInteractionTicksRemaining = point->mDurationTicks;
				break;
			}
		}
	}

	void SimulationCoordinator::moveInteractions(float frameTime)
	{
		for (auto const& [pointId, point] : mBuilding.mInteractionPoints.entries())
		{
			(void)pointId;
			auto request = mBuilding.mInteractionRequests.find(point->mActiveRequest);
			if (!request || request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			auto actor = mBuilding.mAgents.find(request->mActor);
			if (!actor || actor->getSector() != mBuilding.mSectors[(size_t)point->mSector.value - 1].get())
			{
				cancelInteraction(point->mActiveRequest);
				continue;
			}
			if (actor->getGlobalPosition().distanceTo(point->mPosition) > point->mReach)
			{
				actor->moveToPosition(point->mPosition, frameTime);
				continue;
			}
			if (point->mInteractionTicksRemaining > 0)
			{
				--point->mInteractionTicksRemaining;
			}
			if (point->mInteractionTicksRemaining == 0)
			{
				// Reflect the physical press in the rendered control. Auto-reenabling
				// Buttons return to their normal colour after the configured delay.
				pressPhysicalControl(pointId);
				for (auto const& [operationId, requirement] : request->mOperations)
				{
					(void)requirement;
					if (auto operation = mBuilding.mDeviceOperations.find(operationId);
						operation && operation->mState == DeviceOperationState::Pending)
					{
						operation->mActivated = true;
					}
				}
				point->mQueue.erase(remove(point->mQueue.begin(), point->mQueue.end(), point->mActiveRequest), point->mQueue.end());
				point->mActiveRequest = {};
			}
		}
	}

	void SimulationCoordinator::updateInteractionResults()
	{
		for (auto const& [id, request] : mBuilding.mInteractionRequests.entries())
		{
			if (request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			bool waiting = false;
			bool requiredFailure = false;
			bool requiredRejection = false;
			bool bestEffortFailure = false;
			for (auto const& [operationId, requirement] : request->mOperations)
			{
				auto operation = mBuilding.mDeviceOperations.find(operationId);
				if (operation && operation->mState == DeviceOperationState::Rejected)
				{
					if (requirement == InteractionBindingRequirement::Required)
					{
						requiredRejection = true;
					}
					else
					{
						bestEffortFailure = true;
					}
				}
				else if (!operation || operation->mState == DeviceOperationState::Failed
					|| operation->mState == DeviceOperationState::Cancelled)
				{
					(requirement == InteractionBindingRequirement::Required ? requiredFailure : bestEffortFailure) = true;
				}
				else if (operation->mState == DeviceOperationState::Pending || operation->mState == DeviceOperationState::Running)
				{
					waiting = true;
				}
			}
			if (requiredRejection)
			{
				request->mResult = InteractionResult::Rejected;
			}
			else if (requiredFailure)
			{
				request->mResult = InteractionResult::Failed;
			}
			else if (!waiting)
			{
				request->mResult = bestEffortFailure ? InteractionResult::SucceededWithBestEffortFailure : InteractionResult::Succeeded;
			}
			if (request->mResult != InteractionResult::Pending)
			{
				SimulationEvent event;
				event.sequence = mBuilding.mNextEventSequence++;
				event.tick = mBuilding.mSimulationTick;
				event.type = SimulationEventType::InteractionRequestChanged;
				event.phase = mBuilding.mCurrentPhase;
				event.interactionRequest = mBuilding.makeInteractionRequestSnapshot(id, *request);
				mBuilding.mEvents.push_back(std::move(event));
			}
		}
	}

} // core
