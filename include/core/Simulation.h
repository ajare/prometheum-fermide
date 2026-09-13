#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Coordination.h"
#include "core/Vector2.h"


namespace core
{
	enum struct AgentPathState
	{
		Idle,
		MovingToVertex,
		WaitingForTraversal,
		TraversingEdge,
		AwaitingTraversalCommit,
		UnderVertexControl
	};

	struct AgentSnapshot
	{
		AgentId id;
		std::string name;
		SectorId sectorId;
		Vector2 localPosition;
		Vector2 globalPosition;
		AgentPathState state{ AgentPathState::Idle };
		bool hasPath{ false };
		uint32_t targetPathNode{ 0 };
		uint32_t pathNodeCount{ 0 };
		bool hasLocomotionTask{ false };
		TraversalRequestId traversalRequest;
		TraversalPermitId traversalPermit;
		InteractionRequestId interactionRequest;
	};

	struct InteractionPointSnapshot
	{
		InteractionPointId id;
		std::string name;
		SectorId sectorId;
		Vector2 position;
		float reach{ 0.0f };
		uint64_t durationTicks{ 0 };
		InteractionRequestId activeRequest;
	};

	struct InteractionRequestSnapshot
	{
		InteractionRequestId id;
		InteractionPointId point;
		AgentId actor;
		InteractionResult result{ InteractionResult::Pending };
		std::vector<DeviceOperationId> operations;
	};

	struct DeviceOperationSnapshot
	{
		DeviceOperationId id;
		std::string name;
		AgentId requester;
		std::vector<AgentId> requesters;
		bool hasCommand{ false };
		DeviceCommand command;
		DeviceOperationState state{ DeviceOperationState::Pending };
	};

	enum struct DoorSnapshotState { NotADoor, Closed, Opening, Open, Closing };

	struct QueuePositionSnapshot
	{
		uint32_t index{ 0 };
		Vector2 position;
		TraversalRequestId owner;
	};

	struct DoorQueueLaneSnapshot
	{
		SectorId sector;
		Vector2 origin;
		Vector2 direction;
		float extent{ 0.0f };
		std::vector<TraversalRequestId> queue;
		std::vector<QueuePositionSnapshot> positions;
	};

	struct DoorCrossingLaneSnapshot
	{
		uint32_t index{ 0 };
		TraversalRequestId owner;
	};

	struct CapacityPositionSnapshot
	{
		uint32_t index{ 0 };
		Vector2 position;
		AgentId occupant;
		TraversalRequestId admissionReservation;
	};

	struct ShuttleCarriageSnapshot
	{
		uint32_t index{ 0 };
		uint32_t capacity{ 0 };
		uint32_t occupantCount{ 0 };
		uint32_t admissionReservationCount{ 0 };
		std::vector<CapacityPositionSnapshot> positions;
		std::vector<std::vector<TraversalResourceId>> stopDoors;
	};

	struct ShuttleAccessZoneSnapshot
	{
		uint32_t stopIndex{ ~0u };
		uint32_t accessZoneIndex{ ~0u };
		SectorId sector;
		TraversalDirection direction{ TraversalDirection::None };
		std::vector<TraversalRequestId> queue;
	};

	struct TraversalResourceSnapshot
	{
		TraversalResourceId id;
		std::string name;
		bool isDoor{ false };
		bool isLadder{ false };
		bool isForceBridge{ false };
		bool isLift{ false };
		bool isShuttle{ false };
		uint32_t shuttleCapacityPerCarriage{ 0 };
		std::vector<ShuttleCarriageSnapshot> shuttleCarriages;
		std::vector<ShuttleAccessZoneSnapshot> shuttleAccessZones;
		bool liftMoving{ false };
		bool liftAligned{ false };
		bool liftCarDoorOpen{ false };
		LiftStopPhase liftStopPhase{ LiftStopPhase::Idle };
		uint64_t liftServiceStartedTick{ 0 };
		uint64_t liftBoardingCutoffTick{ 0 };
		bool liftAcceptingBoarders{ false };
		bool liftDraining{ false };
		uint32_t liftPendingSafeExits{ 0 };
		uint32_t liftCurrentStop{ 0 };
		uint32_t liftTargetStop{ ~0u };
		TraversalDirection liftDirection{ TraversalDirection::None };
		float liftPosition{ 0.0f };
		SectorId liftSector;
		AgentId liftPassenger;
		TraversalRequestId liftAdmissionReservation;
		uint32_t liftDestinationStop{ ~0u };
		InteractionPointId liftSelector;
		TraversalRequestId liftActiveConfirmation;
		std::vector<TraversalRequestId> liftConfirmationQueue;
		std::vector<uint32_t> liftStopRequestOwnerCounts;
		std::vector<uint64_t> liftStopOldestRequestTicks;
		std::vector<uint32_t> liftScheduledStops;
		bool isExtensible{ false };
		bool extended{ false };
		bool retractionPending{ false };
		uint32_t extensionRequestLeaseCount{ 0 };
		uint32_t extensionOccupantLeaseCount{ 0 };
		bool isNarrowStaircase{ false };
		bool enabled{ true };
		uint32_t capacity{ 0 };
		uint32_t occupantCount{ 0 };
		uint32_t admissionReservationCount{ 0 };
		float agentSpacing{ 0.0f };
		SectorId capacitySector;
		std::vector<TraversalRequestId> admissionQueue;
		std::vector<CapacityPositionSnapshot> capacityPositions;
		TraversalDirection activeDirection{ TraversalDirection::None };
		uint32_t directionalBatchCount{ 0 };
		uint32_t directionalBatchLimit{ 0 };
		uint32_t ascendingWaitingCount{ 0 };
		uint32_t descendingWaitingCount{ 0 };
		DoorActivationMode doorActivationMode{ DoorActivationMode::Unavailable };
		DoorSnapshotState doorState{ DoorSnapshotState::NotADoor };
		float doorOpenPercentage{ 0.0f };
		uint32_t openLeaseCount{ 0 };
		uint32_t preparationLeaseCount{ 0 };
		uint32_t crossingLeaseCount{ 0 };
		uint32_t externalOpenLeaseCount{ 0 };
		bool presenceObserved{ false };
		bool obstructionObserved{ false };
		uint64_t holdOpenTicks{ 0 };
		std::vector<InteractionPointId> controls;
		InteractionRequestId activePreparation;
		TraversalRequestId preparationOperator;
		TraversalRequestId crossingOwner;
		std::vector<DoorCrossingLaneSnapshot> crossingLanes;
		std::vector<DoorQueueLaneSnapshot> queueLanes;
	};

	struct TraversalRequestSnapshot
	{
		TraversalRequestId id;
		AgentId owner;
		EdgeType edgeType{ EdgeType::Location };
		SectorId sourceSector;
		SectorId destinationSector;
		Vector2 sourceEndpoint;
		Vector2 destinationEndpoint;
		TraversalRequestState state{ TraversalRequestState::Pending };
		TraversalResourceId resource;
		DeviceOperationId preparationOperation;
		TraversalPermitId permit;
		TraversalFailureReason failureReason{ TraversalFailureReason::None };
		QueueTicketId queueTicket;
		uint64_t queuedAtTick{ 0 };
		uint32_t queueApproach{ ~0u };
		bool hasQueuePosition{ false };
		uint32_t queuePosition{ ~0u };
		Vector2 queuePositionTarget;
		bool hasCrossingLane{ false };
		uint32_t crossingLane{ ~0u };
		bool hasCapacityPosition{ false };
		uint32_t capacityPosition{ ~0u };
		uint32_t shuttleCarriage{ ~0u };
		uint32_t shuttleAccessZone{ ~0u };
		TraversalResourceId shuttleDoor;
		TraversalDirection direction{ TraversalDirection::None };
		uint64_t positionAssignedAtTick{ 0 };
		uint64_t lastPositionProgressTick{ 0 };
		uint64_t positionRetryAtTick{ 0 };
		uint32_t positionRetryCount{ 0 };
	};

	struct TraversalPermitSnapshot
	{
		TraversalPermitId id;
		TraversalRequestId request;
		AgentId owner;
		TraversalPermitState state{ TraversalPermitState::Active };
		uint64_t expiresAtTick{ 0 };
	};

	struct SimulationSnapshot
	{
		uint64_t tick{ 0 };
		std::vector<AgentSnapshot> agents;
		std::vector<InteractionPointSnapshot> interactionPoints;
		std::vector<InteractionRequestSnapshot> interactionRequests;
		std::vector<DeviceOperationSnapshot> deviceOperations;
		std::vector<TraversalResourceSnapshot> traversalResources;
		std::vector<TraversalRequestSnapshot> traversalRequests;
		std::vector<TraversalPermitSnapshot> traversalPermits;
	};

	// These phases are always entered in declaration order for each fixed tick.
	enum struct SimulationPhase
	{
		None,
		ResourceAdvancement,
		IntentCollection,
		Allocation,
		Movement,
		Commit,
		CleanupAndEventPublication
	};

	enum struct SimulationEventType
	{
		AgentAdded,
		AgentChanged,
		PhaseCompleted,
		AgentRemoved,
		InteractionPointAdded,
		InteractionPointRemoved,
		InteractionRequestAdded,
		InteractionRequestChanged,
		InteractionRequestRemoved,
		DeviceOperationAdded,
		DeviceOperationChanged,
		DeviceOperationRemoved,
		TraversalResourceAdded,
		TraversalResourceRemoved,
		TraversalRequestAdded,
		TraversalRequestChanged,
		TraversalRequestRemoved,
		TraversalPermitAdded,
		TraversalPermitChanged,
		TraversalPermitRemoved
	};

	// Events contain values only.  They are collected during a tick and become
	// visible after cleanup, so consuming them cannot re-enter simulation code.
	struct SimulationEvent
	{
		uint64_t sequence{ 0 };
		uint64_t tick{ 0 };
		SimulationEventType type{ SimulationEventType::PhaseCompleted };
		SimulationPhase phase{ SimulationPhase::None };
		bool hasPreviousAgent{ false };
		AgentSnapshot previousAgent;
		AgentSnapshot agent;
		InteractionPointSnapshot interactionPoint;
		InteractionRequestSnapshot interactionRequest;
		DeviceOperationSnapshot deviceOperation;
		TraversalResourceSnapshot traversalResource;
		TraversalRequestSnapshot traversalRequest;
		TraversalPermitSnapshot traversalPermit;
	};

} // core
