#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/EdgeType.h"
#include "core/EntityId.h"
#include "core/Vector2.h"


namespace core
{
	class Building;
	class Door;
	class Ladder;
	class Staircase;

	enum struct TraversalDirection { None, Ascending, Descending };

	enum struct DoorActivationMode
	{
		Automatic,
		Manual,
		RemoteControlled,
		Unavailable
	};

	enum struct DeviceCommandType
	{
		SetSectorLights,
		OpenDoor
	};

	enum struct DoorOpenLeaseKind
	{
		Preparation,
		Crossing,
		ExternalHoldOpen
	};

	enum struct DoorSensorObservation
	{
		Clear,
		Presence,
		Obstruction
	};

	struct DoorOpenLease
	{
		DoorOpenLeaseKind kind{ DoorOpenLeaseKind::ExternalHoldOpen };
		TraversalRequestId request;
	};

	// A command says which state is desired. It is intentionally not a toggle:
	// retries and equivalent requests are therefore idempotent and coalescible.
	struct DeviceCommand
	{
		DeviceCommandType type{ DeviceCommandType::SetSectorLights };
		SectorId target;
		bool desiredState{ false };
		TraversalResourceId traversalResource;

		friend bool operator==(DeviceCommand const&, DeviceCommand const&) = default;
	};

	enum struct InteractionBindingRequirement
	{
		Required,
		BestEffort
	};

	struct InteractionBinding
	{
		DeviceCommand command;
		InteractionBindingRequirement requirement{ InteractionBindingRequirement::Required };
	};

	enum struct InteractionResult
	{
		Pending,
		Succeeded,
		SucceededWithBestEffortFailure,
		Failed,
		Rejected,
		Cancelled
	};

	class InteractionPoint
	{
		friend class Building;

		std::string mName;
		SectorId mSector;
		Vector2 mPosition;
		float mReach{ 0.25f };
		uint64_t mDurationTicks{ 1 };
		std::vector<InteractionBinding> mBindings;
		std::vector<InteractionRequestId> mQueue;
		InteractionRequestId mActiveRequest;
		uint64_t mInteractionTicksRemaining{ 0 };

		explicit InteractionPoint(std::string name)
			: mName(std::move(name))
		{
		}

		InteractionPoint(std::string name, SectorId sector, Vector2 position, float reach,
			uint64_t durationTicks, std::vector<InteractionBinding> bindings)
			: mName(std::move(name))
			, mSector(sector)
			, mPosition(position)
			, mReach(reach)
			, mDurationTicks(durationTicks)
			, mBindings(std::move(bindings))
		{
		}

	public:
		InteractionPoint(InteractionPoint const&) = delete;
		InteractionPoint& operator=(InteractionPoint const&) = delete;

		std::string const& getName() const { return mName; }
		SectorId getSector() const { return mSector; }
		Vector2 const& getPosition() const { return mPosition; }
		float getReach() const { return mReach; }
		uint64_t getDurationTicks() const { return mDurationTicks; }
		InteractionRequestId getActiveRequest() const { return mActiveRequest; }
	};

	enum struct DeviceOperationState
	{
		Pending,
		Running,
		Succeeded,
		Failed,
		Rejected,
		Cancelled
	};

	class DeviceOperation
	{
		friend class Building;

		std::string mName;
		AgentId mRequester;
		std::set<AgentId> mRequesters;
		DeviceCommand mCommand;
		bool mHasCommand{ false };
		bool mActivated{ false };
		DeviceOperationState mState{ DeviceOperationState::Pending };

		DeviceOperation(std::string name, AgentId requester)
			: mName(std::move(name)), mRequester(requester), mRequesters{ requester }
		{
		}

		DeviceOperation(std::string name, AgentId requester, DeviceCommand command)
			: mName(std::move(name)), mRequester(requester), mRequesters{ requester },
			  mCommand(command), mHasCommand(true)
		{
		}

	public:
		DeviceOperation(DeviceOperation const&) = delete;
		DeviceOperation& operator=(DeviceOperation const&) = delete;

		std::string const& getName() const { return mName; }
		AgentId getRequester() const { return mRequester; }
		std::set<AgentId> const& getRequesters() const { return mRequesters; }
		DeviceCommand const& getCommand() const { return mCommand; }
		bool hasCommand() const { return mHasCommand; }
		DeviceOperationState getState() const { return mState; }
		void setState(DeviceOperationState state) { mState = state; }
	};

	class InteractionRequest
	{
		friend class Building;

		InteractionPointId mPoint;
		AgentId mActor;
		InteractionResult mResult{ InteractionResult::Pending };
		std::vector<std::pair<DeviceOperationId, InteractionBindingRequirement>> mOperations;

		InteractionRequest(InteractionPointId point, AgentId actor)
			: mPoint(point), mActor(actor)
		{
		}

	public:
		InteractionRequest(InteractionRequest const&) = delete;
		InteractionRequest& operator=(InteractionRequest const&) = delete;
		InteractionPointId getPoint() const { return mPoint; }
		AgentId getActor() const { return mActor; }
		InteractionResult getResult() const { return mResult; }
		std::vector<std::pair<DeviceOperationId, InteractionBindingRequirement>> const& getOperations() const { return mOperations; }
	};

	struct DoorQueueLane
	{
		SectorId sector;
		Vector2 origin;
		Vector2 direction;
		float extent{ 0.0f };
		std::vector<Vector2> positions;
		std::vector<TraversalRequestId> positionOwners;
		std::vector<TraversalRequestId> queue;
	};

	class TraversalResource
	{
		friend class Building;
		std::string mName;
		std::shared_ptr<Door> mDoor;
		std::shared_ptr<Ladder> mLadder;
		std::shared_ptr<Staircase> mStaircase;
		SectorId mLadderSector;
		float mLadderSpacing{ 0.0f };
		uint32_t mCapacity{ 0 };
		std::vector<Vector2> mCapacityPositions;
		std::vector<AgentId> mOccupants;
		std::vector<TraversalRequestId> mAdmissionReservations;
		std::vector<TraversalRequestId> mAdmissionQueue;
		TraversalDirection mActiveDirection{ TraversalDirection::None };
		uint32_t mDirectionalBatchCount{ 0 };
		uint32_t mDirectionalBatchLimit{ 1 };
		DoorActivationMode mDoorActivationMode{ DoorActivationMode::Unavailable };
		uint64_t mHoldOpenTicks{ 0 };
		bool mEnabled{ true };
		std::map<DoorOpenLeaseId, DoorOpenLease> mOpenLeases;
		std::map<DoorSensorId, DoorSensorObservation> mSensorObservations;
		std::vector<InteractionPointId> mControls;
		InteractionRequestId mActivePreparation;
		TraversalRequestId mPreparationOperator;
		DeviceOperationId mSharedPreparationOperation;
		uint32_t mPreparationAttempts{ 0 };
		uint64_t mNextPreparationTick{ 0 };
		std::array<DoorQueueLane, 2> mQueueLanes;
		std::vector<TraversalRequestId> mCrossingOwners;
		explicit TraversalResource(std::string name) : mName(std::move(name)) {}
		TraversalResource(std::string name, std::shared_ptr<Door> door,
			DoorActivationMode mode, uint64_t holdOpenTicks)
			: mName(std::move(name)), mDoor(std::move(door)),
			  mDoorActivationMode(mode), mHoldOpenTicks(holdOpenTicks) {}
		TraversalResource(std::string name, std::shared_ptr<Ladder> ladder,
			SectorId ladderSector, float spacing, uint32_t capacity,
			uint32_t batchLimit, std::vector<Vector2> positions)
			: mName(std::move(name)), mLadder(std::move(ladder)),
			  mLadderSector(ladderSector), mLadderSpacing(spacing), mCapacity(capacity),
			  mCapacityPositions(std::move(positions)), mOccupants(capacity),
			  mAdmissionReservations(capacity), mDirectionalBatchLimit(batchLimit) {}
		TraversalResource(std::string name, std::shared_ptr<Staircase> staircase,
			SectorId staircaseSector, uint32_t capacity, uint32_t batchLimit,
			std::vector<Vector2> positions)
			: mName(std::move(name)), mStaircase(std::move(staircase)),
			  mLadderSector(staircaseSector), mCapacity(capacity),
			  mCapacityPositions(std::move(positions)), mOccupants(capacity),
			  mAdmissionReservations(capacity), mDirectionalBatchLimit(batchLimit) {}
	public:
		TraversalResource(TraversalResource const&) = delete;
		TraversalResource& operator=(TraversalResource const&) = delete;
		std::string const& getName() const { return mName; }
		bool isDoor() const { return mDoor != nullptr; }
		bool isLadder() const { return mLadder != nullptr; }
		bool isNarrowStaircase() const { return mStaircase != nullptr; }
		bool isEnabled() const { return mEnabled; }
		uint32_t getCapacity() const { return mCapacity; }
		SectorId getLadderSector() const { return mLadderSector; }
		DoorActivationMode getDoorActivationMode() const { return mDoorActivationMode; }
		std::vector<InteractionPointId> const& getControls() const { return mControls; }
	};

	enum struct TraversalRequestState { Pending, Granted, Denied, Cancelled, Committed };

	enum struct TraversalFailureReason
	{
		None,
		NoReachableControl,
		ControlRejected,
		PreparationFailed,
		ResourceDisabled,
		LocalGoalUnreachable,
		PermitExpired
	};

	// Tick-based policy keeps timeout and replanning behaviour deterministic and
	// lets headless scenarios shorten the otherwise conservative production values.
	struct TraversalWaitingPolicy
	{
		uint64_t localGoalTimeoutTicks{ 180 };
		uint64_t localGoalRetryDelayTicks{ 6 };
		uint32_t maximumLocalGoalRetries{ 3 };
		uint64_t permitProgressTimeoutTicks{ 120 };
		uint64_t minimumReplanWaitTicks{ 300 };
		uint64_t replanIntervalTicks{ 120 };
		float replanEtaMarginSeconds{ 2.0f };
		float queueDelayPerAgentSeconds{ 1.0f };
	};

	class TraversalRequest
	{
		friend class Building;
		AgentId mOwner;
		EdgeType mEdgeType;
		SectorId mSourceSector;
		SectorId mDestinationSector;
		Vector2 mSourceEndpoint;
		Vector2 mDestinationEndpoint;
		TraversalRequestState mState{ TraversalRequestState::Pending };
		bool mPreparationRequested{ false };
		TraversalResourceId mResource;
		DeviceOperationId mPreparationOperation;
		TraversalPermitId mPermit;
		TraversalFailureReason mFailureReason{ TraversalFailureReason::None };
		QueueTicketId mQueueTicket;
		uint64_t mQueuedAtTick{ 0 };
		uint32_t mQueueApproach{ ~0u };
		uint32_t mQueuePosition{ ~0u };
		uint64_t mPositionAssignedAtTick{ 0 };
		uint64_t mLastPositionProgressTick{ 0 };
		float mBestPositionDistance{ 0.0f };
		uint64_t mPositionRetryAtTick{ 0 };
		uint32_t mPositionRetryCount{ 0 };
		uint32_t mCrossingLane{ ~0u };
		uint32_t mCapacityPosition{ ~0u };
		TraversalDirection mDirection{ TraversalDirection::None };
		DoorOpenLeaseId mPreparationLease;
		DoorOpenLeaseId mCrossingLease;
		TraversalRequest(AgentId owner, EdgeType edgeType, SectorId sourceSector,
			SectorId destinationSector, Vector2 sourceEndpoint, Vector2 destinationEndpoint)
			: mOwner(owner), mEdgeType(edgeType), mSourceSector(sourceSector),
			  mDestinationSector(destinationSector), mSourceEndpoint(sourceEndpoint),
			  mDestinationEndpoint(destinationEndpoint) {}
	public:
		TraversalRequest(TraversalRequest const&) = delete;
		TraversalRequest& operator=(TraversalRequest const&) = delete;
		AgentId getOwner() const { return mOwner; }
		EdgeType getEdgeType() const { return mEdgeType; }
		SectorId getSourceSector() const { return mSourceSector; }
		SectorId getDestinationSector() const { return mDestinationSector; }
		Vector2 const& getSourceEndpoint() const { return mSourceEndpoint; }
		Vector2 const& getDestinationEndpoint() const { return mDestinationEndpoint; }
		TraversalRequestState getState() const { return mState; }
		bool wasPreparationRequested() const { return mPreparationRequested; }
		TraversalResourceId getResource() const { return mResource; }
		DeviceOperationId getPreparationOperation() const { return mPreparationOperation; }
		TraversalPermitId getPermit() const { return mPermit; }
		TraversalFailureReason getFailureReason() const { return mFailureReason; }
		QueueTicketId getQueueTicket() const { return mQueueTicket; }
		uint64_t getQueuedAtTick() const { return mQueuedAtTick; }
		uint32_t getQueueApproach() const { return mQueueApproach; }
		bool hasQueuePosition() const { return mQueuePosition != ~0u; }
		uint32_t getQueuePosition() const { return mQueuePosition; }
		bool hasCrossingLane() const { return mCrossingLane != ~0u; }
		uint32_t getCrossingLane() const { return mCrossingLane; }
		bool hasCapacityPosition() const { return mCapacityPosition != ~0u; }
		uint32_t getCapacityPosition() const { return mCapacityPosition; }
		TraversalDirection getDirection() const { return mDirection; }
	};

	enum struct TraversalPermitState { Active, Committed, Cancelled };

	class TraversalPermit
	{
		friend class Building;
		TraversalRequestId mRequest;
		AgentId mOwner;
		TraversalPermitState mState{ TraversalPermitState::Active };
		uint64_t mExpiresAtTick{ 0 };
		float mBestDestinationDistance{ 0.0f };
		TraversalPermit(TraversalRequestId request, AgentId owner) : mRequest(request), mOwner(owner) {}
	public:
		TraversalPermit(TraversalPermit const&) = delete;
		TraversalPermit& operator=(TraversalPermit const&) = delete;
		TraversalRequestId getRequest() const { return mRequest; }
		AgentId getOwner() const { return mOwner; }
		TraversalPermitState getState() const { return mState; }
		uint64_t getExpiresAtTick() const { return mExpiresAtTick; }
	};

	template<typename Entity>
	struct EntityLookup
	{
		Entity* entity{ nullptr };
		std::string diagnostic;
		explicit operator bool() const { return entity != nullptr; }
	};

	struct EntityRemovalResult
	{
		bool removed{ false };
		std::string diagnostic;
		explicit operator bool() const { return removed; }
	};

} // core
