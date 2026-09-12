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
	};

	struct InteractionPointSnapshot
	{
		InteractionPointId id;
		std::string name;
	};

	struct DeviceOperationSnapshot
	{
		DeviceOperationId id;
		std::string name;
		AgentId requester;
		DeviceOperationState state{ DeviceOperationState::Pending };
	};

	struct TraversalResourceSnapshot
	{
		TraversalResourceId id;
		std::string name;
	};

	struct SimulationSnapshot
	{
		uint64_t tick{ 0 };
		std::vector<AgentSnapshot> agents;
		std::vector<InteractionPointSnapshot> interactionPoints;
		std::vector<DeviceOperationSnapshot> deviceOperations;
		std::vector<TraversalResourceSnapshot> traversalResources;
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
		DeviceOperationAdded,
		DeviceOperationChanged,
		DeviceOperationRemoved,
		TraversalResourceAdded,
		TraversalResourceRemoved
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
		DeviceOperationSnapshot deviceOperation;
		TraversalResourceSnapshot traversalResource;
	};

} // core
