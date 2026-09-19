#include "core/SimulationCoordinator.h"

#include "core/Building.h"
#include "core/Coordination.h"
#include "core/Door.h"
#include "core/ExtensibleObject.h"


namespace core
{

	using namespace std;

	// Door and extensible traversal preparation moved out of Building
	// (ADR 0004 stage 3). The behaviour is unchanged: the coordinator works on
	// Building's traversal-request, interaction and device-operation registries
	// through friendship and calls its own queue refresh, ladder admission,
	// grant and denial machinery directly - that queue and admission core joined
	// the coordinator in stage 4, so no facade callback is left in this seam.

	void SimulationCoordinator::allocateRemoteDoorPreparation(TraversalRequestId requestId, TraversalResource& resource)
	{
		constexpr uint32_t MaximumPreparationAttempts = 2;
		constexpr uint64_t RetryDelayTicks = 3;

		auto request = mBuilding.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending)
		{
			return;
		}

		auto applicableControl = [&](TraversalRequest const& candidate) -> InteractionPointId
		{
			for (auto pointId : resource.mControls)
			{
				auto point = mBuilding.mInteractionPoints.find(pointId);
				if (point && point->mSector == candidate.mSourceSector)
				{
					return pointId;
				}
			}
			return {};
		};

		if (!applicableControl(*request))
		{
			denyTraversalRequest(requestId, TraversalFailureReason::NoReachableControl);
			return;
		}
		// An opportunistic press may already have started opening the Door. Wait for
		// that idempotent command rather than assigning another physical operator.
		if (resource.mDoor->isOpening() && !resource.mActivePreparation) return;

		bool failedPreparation = false;
		bool completedPreparation = false;
		if (resource.mActivePreparation)
		{
			auto active = mBuilding.mInteractionRequests.find(resource.mActivePreparation);
			if (active && active->mResult == InteractionResult::Pending)
			{
				bool interactionStarted = false;
				for (auto const& [operationId, requirement] : active->mOperations)
				{
					(void)requirement;
					if (auto operation = mBuilding.mDeviceOperations.find(operationId))
					{
						interactionStarted = interactionStarted || operation->mActivated;
						operation->mRequesters.insert(request->mOwner);
						if (!request->mPreparationOperation)
						{
							request->mPreparationOperation = operationId;
						}
					}
				}
				request->mPreparationRequested = true;

				// If some other source achieved the desired state before the operator
				// touched the control, release its physical reservation immediately.
				if (resource.mDoor->isOpen() && !interactionStarted)
				{
					cancelInteraction(resource.mActivePreparation);
					resource.mActivePreparation = {};
					resource.mPreparationOperator = {};
					resource.mSharedPreparationOperation = {};
					refreshQueuePositions(resource);
					tryGrantDoorQueue(resource);
				}
				return;
			}

			if (active && active->mResult == InteractionResult::Rejected)
			{
				resource.mActivePreparation = {};
				resource.mPreparationOperator = {};
				resource.mSharedPreparationOperation = {};
				denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
				return;
			}
			if (active && (active->mResult == InteractionResult::Succeeded
				|| active->mResult == InteractionResult::SucceededWithBestEffortFailure))
			{
				completedPreparation = true;
				resource.mPreparationAttempts = 0;
			}
			if (active && active->mResult == InteractionResult::Failed)
			{
				failedPreparation = true;
				++resource.mPreparationAttempts;
				resource.mNextPreparationTick = mBuilding.mSimulationTick + RetryDelayTicks;
			}
			resource.mActivePreparation = {};
			resource.mPreparationOperator = {};
			resource.mSharedPreparationOperation = {};
			refreshQueuePositions(resource);
		}

		if (resource.mDoor->isOpen() && !failedPreparation
			&& (resource.mPreparationAttempts == 0 || completedPreparation))
		{
			tryGrantDoorQueue(resource);
			return;
		}

		if (resource.mPreparationAttempts >= MaximumPreparationAttempts)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
			return;
		}
		if (mBuilding.mSimulationTick < resource.mNextPreparationTick)
		{
			return; // A temporary block uses a stable, tick-based retry delay.
		}

		TraversalRequestId selected;
		InteractionPointId selectedControl;
		for (auto const& [candidateId, candidate] : mBuilding.mTraversalRequests.entries())
		{
			if (candidate->mResource != request->mResource
				|| candidate->mState != TraversalRequestState::Pending)
			{
				continue;
			}
			auto control = applicableControl(*candidate);
			if (control && (!selected || candidateId < selected))
			{
				selected = candidateId;
				selectedControl = control;
			}
		}
		if (selected != requestId)
		{
			return;
		}

		auto interactionId = requestInteractionForTraversal(selectedControl, request->mOwner);
		if (!interactionId)
		{
			// Another locomotion/interaction task can make the control temporarily
			// busy. Do not turn that scheduling condition into permanent rejection.
			resource.mNextPreparationTick = mBuilding.mSimulationTick + RetryDelayTicks;
			return;
		}
		auto interaction = mBuilding.mInteractionRequests.find(interactionId);
		if (!interaction || interaction->mOperations.empty())
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
			return;
		}

		resource.mActivePreparation = interactionId;
		resource.mPreparationOperator = requestId;
		refreshQueuePositions(resource);
		resource.mSharedPreparationOperation = interaction->mOperations.front().first;
		for (auto const& [candidateId, candidate] : mBuilding.mTraversalRequests.entries())
		{
			(void)candidateId;
			if (candidate->mResource != request->mResource
				|| candidate->mState != TraversalRequestState::Pending)
			{
				continue;
			}
			candidate->mPreparationRequested = true;
			candidate->mPreparationOperation = resource.mSharedPreparationOperation;
			for (auto const& [operationId, requirement] : interaction->mOperations)
			{
				(void)requirement;
				if (auto operation = mBuilding.mDeviceOperations.find(operationId))
				{
					operation->mRequesters.insert(candidate->mOwner);
				}
			}
		}
	}

	void SimulationCoordinator::allocateExtensiblePreparation(TraversalRequestId requestId, TraversalResource& resource)
	{
		auto request = mBuilding.mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		if (!resource.mEnabled)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
			return;
		}
		if (resource.mExtensible->isExtended())
		{
			if (resource.mLadder)
			{
				resource.mActivePreparation = {};
				resource.mPreparationOperator = {};
				resource.mSharedPreparationOperation = {};
				refreshQueuePositions(resource);
				attachLadderAdmissionRequest(requestId, resource);
				tryGrantLadderAdmissions(resource);
			}
			else if (resource.mForceBridge)
			{
				resource.mActivePreparation = {};
				resource.mPreparationOperator = {};
				resource.mSharedPreparationOperation = {};
				refreshQueuePositions(resource);
				tryGrantDoorQueue(resource);
			}
			else grantTraversalRequest(requestId);
			return;
		}

		auto controlFor = [&](TraversalRequest const& candidate)
		{
			for (auto pointId : resource.mControls)
				if (auto point = mBuilding.mInteractionPoints.find(pointId); point && point->mSector == candidate.mSourceSector)
					return pointId;
			return InteractionPointId{};
		};
		if (!controlFor(*request))
		{
			denyTraversalRequest(requestId, TraversalFailureReason::NoReachableControl);
			return;
		}

		if (resource.mActivePreparation)
		{
			auto active = mBuilding.mInteractionRequests.find(resource.mActivePreparation);
			if (active && active->mResult == InteractionResult::Pending)
			{
				request->mPreparationRequested = true;
				for (auto const& [operationId, requirement] : active->mOperations)
				{
					(void)requirement;
					request->mPreparationOperation = operationId;
					if (auto operation = mBuilding.mDeviceOperations.find(operationId))
						operation->mRequesters.insert(request->mOwner);
				}
				return;
			}
			if (active && (active->mResult == InteractionResult::Failed
				|| active->mResult == InteractionResult::Rejected))
			{
				denyTraversalRequest(requestId, active->mResult == InteractionResult::Rejected
					? TraversalFailureReason::ControlRejected : TraversalFailureReason::PreparationFailed);
			}
			resource.mActivePreparation = {};
			resource.mPreparationOperator = {};
			resource.mSharedPreparationOperation = {};
			if (resource.mLadder || resource.mForceBridge) refreshQueuePositions(resource);
			if (request->mState != TraversalRequestState::Pending) return;
			if (resource.mExtensible->isExtended())
			{
				if (resource.mLadder) { attachLadderAdmissionRequest(requestId, resource); tryGrantLadderAdmissions(resource); }
				else if (resource.mForceBridge) tryGrantDoorQueue(resource);
				else grantTraversalRequest(requestId);
				return;
			}
		}

		TraversalRequestId selected;
		for (auto const& [candidateId, candidate] : mBuilding.mTraversalRequests.entries())
			if (candidate->mResource == request->mResource
				&& candidate->mState == TraversalRequestState::Pending && controlFor(*candidate)
				&& (!selected || candidateId < selected)) selected = candidateId;
		if (selected != requestId) return;
		auto interactionId = requestInteractionForTraversal(controlFor(*request), request->mOwner);
		if (!interactionId) return;
		auto interaction = mBuilding.mInteractionRequests.find(interactionId);
		if (!interaction || interaction->mOperations.empty())
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
			return;
		}
		resource.mActivePreparation = interactionId;
		resource.mPreparationOperator = requestId;
		resource.mSharedPreparationOperation = interaction->mOperations.front().first;
		if (resource.mLadder || resource.mForceBridge) refreshQueuePositions(resource);
		request->mPreparationRequested = true;
		request->mPreparationOperation = resource.mSharedPreparationOperation;
	}

	DoorOpenLeaseId SimulationCoordinator::acquireDoorOpenLease(TraversalResource& resource,
		DoorOpenLeaseKind kind, TraversalRequestId request)
	{
		auto id = DoorOpenLeaseId{ mBuilding.mNextDoorOpenLeaseValue++ };
		resource.mOpenLeases.emplace(id, DoorOpenLease{ kind, request });
		resource.mDoor->acquireOpenLease();
		// Safety and locally activated preparation have priority over a close.
		// Remote preparation still has to reach its configured physical control.
		if (resource.mDoor->isClosing()
			&& (kind != DoorOpenLeaseKind::Preparation
				|| resource.mDoorActivationMode != DoorActivationMode::RemoteControlled))
		{
			resource.mDoor->requestOpen();
		}
		return id;
	}

	bool SimulationCoordinator::releaseDoorOpenLease(TraversalResource& resource, DoorOpenLeaseId lease)
	{
		if (!lease || resource.mOpenLeases.erase(lease) == 0) return false;
		resource.mDoor->releaseOpenLease();
		return true;
	}

	DoorOpenLeaseId SimulationCoordinator::acquireDoorOpenLease(TraversalResourceId resourceId, DoorOpenLeaseKind kind)
	{
		auto resource = mBuilding.mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor || !resource->mEnabled) return {};
		return acquireDoorOpenLease(*resource, kind);
	}

	bool SimulationCoordinator::releaseDoorOpenLease(TraversalResourceId resourceId, DoorOpenLeaseId lease)
	{
		auto resource = mBuilding.mTraversalResources.find(resourceId);
		return resource && resource->mDoor && releaseDoorOpenLease(*resource, lease);
	}

} // core
