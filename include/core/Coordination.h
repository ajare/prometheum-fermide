#pragma once

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
	class ExtensibleObject;
	class ForceBridge;
	class Ladder;
	class Lift;
	class Shuttle;
	class SimulationCoordinator;
	class Stairwell;
	class Window;

	enum struct TraversalDirection { None, Ascending, Descending };

	enum struct LiftStopPhase
	{
		Idle,
		Moving,
		Opening,
		Disembarking,
		Boarding,
		Closing
	};

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
		OpenDoor,
		SetExtendedState,
		CallLift,
		SelectLiftDestination,
		CallShuttle,
		SelectShuttleDestination
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
		TraversalResourceId traversalResource{};
		uint32_t stopIndex{ ~0u };

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
		friend class SimulationCoordinator;

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
		friend class SimulationCoordinator;

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
		friend class SimulationCoordinator;

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

	struct LiftStop
	{
		SectorId locationSector;
		float globalPosition{ 0.0f };
		TraversalResourceId landingResource;
		InteractionPointId callControl;
	};

	struct LiftTripIntent
	{
		uint32_t originStop{ ~0u };
		uint32_t destinationStop{ ~0u };
		uint64_t registeredAtTick{ 0 };
	};

	// A coupled shuttle has one vehicle schedule, but each physical carriage owns
	// a disjoint range of the coordinator's standing and reservation slots.
	struct ShuttleCarriage
	{
		uint32_t index{ 0 };
		uint32_t firstCapacityPosition{ 0 };
		uint32_t capacity{ 0 };
		std::vector<std::vector<TraversalResourceId>> stopDoors;
	};

	struct ShuttleDoor
	{
		uint32_t stopIndex{ ~0u };
		uint32_t carriageIndex{ ~0u };
		uint32_t accessZoneIndex{ ~0u };
		SectorId locationSector;
		TraversalResourceId landingResource;
	};

	struct QueueLane
	{
		SectorId sector;
		Vector2 origin;
		Vector2 direction;
		float extent{ 0.0f };
		std::vector<Vector2> positions;
		std::vector<TraversalRequestId> positionOwners;
		std::vector<TraversalRequestId> queue;
	};

	enum struct TraversalFailureReason;

	class TraversalResource
	{
		friend class Building;
		friend class SimulationCoordinator;
		std::string mName;
		std::shared_ptr<Door> mDoor;
		std::shared_ptr<Window> mWindow;
		std::shared_ptr<ExtensibleObject> mExtensible;
		std::shared_ptr<ForceBridge> mForceBridge;
		std::shared_ptr<Ladder> mLadder;
		std::shared_ptr<Lift> mLift;
		std::shared_ptr<Shuttle> mShuttle;
		std::shared_ptr<Stairwell> mStairwell;
		// Lift coordinators are separate from their landing-door resources. The
		// latter point back to the coordinator and one stop.
		TraversalResourceId mLiftCoordinator;
		uint32_t mLiftStopIndex{ ~0u };
		SectorId mLiftSector;
		std::vector<LiftStop> mLiftStops;
		InteractionPointId mLiftSelector;
		float mLiftPosition{ 0.0f };
		uint32_t mLiftCurrentStop{ 0 };
		uint32_t mLiftTargetStop{ ~0u };
		TraversalDirection mLiftDirection{ TraversalDirection::None };
		bool mLiftMoving{ false };
		bool mLiftCarDoorOpen{ false };
		// Open platform lifts share the transport scheduler but replace landing/car
		// doors with one capacity-limited virtual crossing boundary.
		bool mOpenPlatformLift{ false };
		std::vector<TraversalRequestId> mVirtualBoundaryOwners;
		// Callers that missed an exact PlatformLift cutoff remain physically still
		// until that car begins another boarding window at their stop.
		std::set<TraversalRequestId> mOpenPlatformMissedBoarding;
		std::map<TraversalRequestId, Vector2> mOpenPlatformMissedPositions;
		LiftStopPhase mLiftStopPhase{ LiftStopPhase::Idle };
		uint64_t mLiftServiceStartedTick{ 0 };
		uint64_t mLiftBoardingCutoffTick{ 0 };
		uint64_t mLiftMinimumDwellTicks{ 0 };
		uint64_t mLiftMaximumBoardingTicks{ 0 };
		// A disabled moving lift drains to its next aligned stop. Passengers whose
		// route is cancelled or whose selector fails are likewise retained until a
		// landing can safely accept their disembark traversal.
		bool mLiftDraining{ false };
		std::set<AgentId> mLiftExitAtSafeStop;
		std::map<AgentId, TraversalFailureReason> mLiftExitFailures;
		std::map<AgentId, uint32_t> mLiftPassengerDestinations;
		std::map<AgentId, LiftTripIntent> mLiftTripIntents;
		std::vector<std::set<AgentId>> mLiftStopRequestOwners;
		std::vector<std::map<AgentId, uint64_t>> mLiftStopRequestTicks;
		std::vector<TraversalRequestId> mLiftConfirmationQueue;
		TraversalRequestId mLiftActiveConfirmation;
		// Shuttle motion and scheduling remain vehicle-wide. These records partition
		// capacity and associate every stop threshold with a carriage/access zone.
		uint32_t mShuttleCapacityPerCarriage{ 0 };
		std::vector<ShuttleCarriage> mShuttleCarriages;
		std::vector<ShuttleDoor> mShuttleDoors;
		// Compatibility aliases expose the first passenger/reservation in old snapshots.
		AgentId mLiftPassenger;
		TraversalRequestId mLiftAdmissionReservation;
		uint32_t mLiftDestinationStop{ ~0u };
		SectorId mLadderSector;
		float mLadderSpacing{ 0.0f };
		uint32_t mCapacity{ 0 };
		std::vector<Vector2> mCapacityPositions;
		std::vector<AgentId> mOccupants;
		std::vector<TraversalRequestId> mAdmissionReservations;
		std::vector<TraversalRequestId> mAdmissionQueue;
		// Request leases cover preparation, admission, and active crossings;
		// occupant leases persist independently after a ladder entry commits.
		std::set<TraversalRequestId> mExtensionRequestLeases;
		std::set<AgentId> mExtensionOccupantLeases;
		bool mRetractionPending{ false };
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
		// Most thresholds have two approaches. Open platform lifts instead own one
		// physical waiting lane per stop, all using the same queue allocator.
		std::vector<QueueLane> mQueueLanes{ 2 };
		std::vector<TraversalRequestId> mCrossingOwners;
		explicit TraversalResource(std::string name) : mName(std::move(name)) {}
		TraversalResource(std::string name, std::shared_ptr<Door> door,
			DoorActivationMode mode, uint64_t holdOpenTicks)
			: mName(std::move(name)), mDoor(std::move(door)),
			  mDoorActivationMode(mode), mHoldOpenTicks(holdOpenTicks) {}
		TraversalResource(std::string name, std::shared_ptr<Window> window)
			: mName(std::move(name)), mWindow(std::move(window)) {}
		TraversalResource(std::string name, std::shared_ptr<Ladder> ladder,
			std::shared_ptr<ExtensibleObject> extensible, SectorId ladderSector,
			float spacing, uint32_t capacity, uint32_t batchLimit, std::vector<Vector2> positions)
			: mName(std::move(name)), mExtensible(std::move(extensible)), mLadder(std::move(ladder)),
			  mLadderSector(ladderSector), mLadderSpacing(spacing), mCapacity(capacity),
			  mCapacityPositions(std::move(positions)), mOccupants(capacity),
			  mAdmissionReservations(capacity), mDirectionalBatchLimit(batchLimit) {}
		TraversalResource(std::string name, std::shared_ptr<ForceBridge> forceBridge,
			std::shared_ptr<ExtensibleObject> extensible)
			: mName(std::move(name)), mExtensible(std::move(extensible)), mForceBridge(std::move(forceBridge)) {}
		TraversalResource(std::string name, std::shared_ptr<Lift> lift,
			SectorId liftSector, std::vector<LiftStop> stops, uint32_t capacity,
			uint64_t minimumDwellTicks, uint64_t maximumBoardingTicks,
			std::vector<Vector2> positions)
			: mName(std::move(name)), mLift(std::move(lift)), mLiftSector(liftSector),
			  mLiftStops(std::move(stops)), mLiftMinimumDwellTicks(minimumDwellTicks),
			  mLiftMaximumBoardingTicks(maximumBoardingTicks),
			  mLiftStopRequestOwners(mLiftStops.size()), mLiftStopRequestTicks(mLiftStops.size()), mCapacity(capacity),
			  mCapacityPositions(std::move(positions)), mOccupants(capacity),
			  mAdmissionReservations(capacity)
		{
			if (!mLiftStops.empty()) mLiftPosition = mLiftStops.front().globalPosition;
		}
		TraversalResource(std::string name, std::shared_ptr<Shuttle> shuttle,
			SectorId shuttleSector, std::vector<LiftStop> stops, uint32_t capacityPerCarriage,
			uint64_t minimumDwellTicks, uint64_t maximumBoardingTicks,
			std::vector<Vector2> positions)
			: mName(std::move(name)), mShuttle(std::move(shuttle)), mLiftSector(shuttleSector),
			  mLiftStops(std::move(stops)), mLiftMinimumDwellTicks(minimumDwellTicks),
			  mLiftMaximumBoardingTicks(maximumBoardingTicks),
			  mLiftStopRequestOwners(mLiftStops.size()), mLiftStopRequestTicks(mLiftStops.size()),
			  mShuttleCapacityPerCarriage(capacityPerCarriage), mCapacity((uint32_t)positions.size()),
			  mCapacityPositions(std::move(positions)), mOccupants(mCapacity),
			  mAdmissionReservations(mCapacity)
		{
			if (!mLiftStops.empty()) mLiftPosition = mLiftStops.front().globalPosition;
		}
		TraversalResource(std::string name, std::shared_ptr<Stairwell> stairwell,
			SectorId stairwellSector, uint32_t capacity, uint32_t batchLimit,
			std::vector<Vector2> positions)
			: mName(std::move(name)), mStairwell(std::move(stairwell)),
			  mLadderSector(stairwellSector), mCapacity(capacity),
			  mCapacityPositions(std::move(positions)), mOccupants(capacity),
			  mAdmissionReservations(capacity), mDirectionalBatchLimit(batchLimit) {}
	public:
		TraversalResource(TraversalResource const&) = delete;
		TraversalResource& operator=(TraversalResource const&) = delete;
		std::string const& getName() const { return mName; }
		bool isDoor() const { return mDoor != nullptr; }
		bool isWindow() const { return mWindow != nullptr; }
		bool isLadder() const { return mLadder != nullptr; }
		bool isForceBridge() const { return mForceBridge != nullptr; }
		bool isLift() const { return mLift != nullptr; }
		bool isOpenPlatformLift() const { return mOpenPlatformLift; }
		bool isShuttle() const { return mShuttle != nullptr; }
		bool isExtensible() const { return mExtensible != nullptr; }
		bool isNarrowStairwell() const { return mStairwell != nullptr; }
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
		uint64_t destinationRetryDelayTicks{ 3 };
		uint32_t maximumDestinationRetries{ 2 };
		float replanEtaMarginSeconds{ 2.0f };
		float queueDelayPerAgentSeconds{ 1.0f };
	};

	class TraversalRequest
	{
		friend class Building;
		friend class SimulationCoordinator;
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
		uint32_t mPreparationAttempts{ 0 };
		uint64_t mNextPreparationTick{ 0 };
		TraversalPermitId mPermit;
		TraversalFailureReason mFailureReason{ TraversalFailureReason::None };
		QueueTicketId mQueueTicket;
		uint64_t mQueuedAtTick{ 0 };
		uint32_t mQueueApproach{ ~0u };
		uint32_t mQueuePosition{ ~0u };
		// Initial assignment uses the position from which the Agent approached the
		// endpoint. Later reshuffles use its live position to preserve stable spots.
		Vector2 mQueueSelectionPosition;
		int mPreferredQueueSide{ 0 };
		bool mHasHeldQueuePosition{ false };
		uint64_t mPositionAssignedAtTick{ 0 };
		uint64_t mLastPositionProgressTick{ 0 };
		float mBestPositionDistance{ 0.0f };
		uint64_t mPositionRetryAtTick{ 0 };
		uint32_t mPositionRetryCount{ 0 };
		uint32_t mCrossingLane{ ~0u };
		uint32_t mCapacityPosition{ ~0u };
		uint32_t mShuttleCarriage{ ~0u };
		uint32_t mShuttleAccessZone{ ~0u };
		TraversalResourceId mShuttleDoor;
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
		uint32_t getShuttleCarriage() const { return mShuttleCarriage; }
		uint32_t getShuttleAccessZone() const { return mShuttleAccessZone; }
		TraversalResourceId getShuttleDoor() const { return mShuttleDoor; }
		TraversalDirection getDirection() const { return mDirection; }
	};

	enum struct TraversalPermitState { Active, Committed, Cancelled };

	class TraversalPermit
	{
		friend class Building;
		friend class SimulationCoordinator;
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
