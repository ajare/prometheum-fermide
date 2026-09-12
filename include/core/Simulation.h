#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Vector2.h"


namespace core
{
	// Stable value identities used at the public simulation boundary.  Zero is
	// reserved for "no entity"; IDs are assigned monotonically by Building.
	struct AgentId
	{
		uint64_t value{ 0 };

		friend bool operator==(AgentId const&, AgentId const&) = default;
		friend bool operator<(AgentId const& lhs, AgentId const& rhs)
		{
			return lhs.value < rhs.value;
		}
	};

	struct SectorId
	{
		uint64_t value{ 0 };

		friend bool operator==(SectorId const&, SectorId const&) = default;
	};

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

	struct SimulationSnapshot
	{
		uint64_t tick{ 0 };
		std::vector<AgentSnapshot> agents;
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
		PhaseCompleted
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
	};

} // core
