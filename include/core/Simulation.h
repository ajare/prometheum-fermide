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

	struct TraversalResourceSnapshot
	{
		TraversalResourceId id;
		std::string name;
		bool isDoor{ false };
		DoorActivationMode doorActivationMode{ DoorActivationMode::Unavailable };
		DoorSnapshotState doorState{ DoorSnapshotState::NotADoor };
		float doorOpenPercentage{ 0.0f };
		uint32_t openLeaseCount{ 0 };
		uint64_t holdOpenTicks{ 0 };
		std::vector<InteractionPointId> controls;
		InteractionRequestId activePreparation;
		TraversalRequestId preparationOperator;
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
	};

	struct TraversalPermitSnapshot
	{
		TraversalPermitId id;
		TraversalRequestId request;
		AgentId owner;
		TraversalPermitState state{ TraversalPermitState::Active };
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
