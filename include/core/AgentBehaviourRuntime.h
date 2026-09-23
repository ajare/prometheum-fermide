#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <map>
#include <string_view>
#include <vector>

#include "core/AgentBehaviour.h"
#include "core/EntityId.h"

namespace core
{
	class AgentBehaviourRegistry;
	class Building;
	struct SimulationEvent;

	// Application-owned limits. Behaviour code cannot inspect or alter them.
	// One live allocator budget belongs to one Building; the instruction budget
	// is restarted for each protected module load, factory, and callback.
	struct AgentBehaviourRuntimeLimits
	{
		size_t memoryBytes{ 64u * 1024u * 1024u };
		uint32_t instructionsPerCall{ 100'000u };
		uint32_t timersPerInstance{ 256u };
		uint32_t callbacksPerBoundary{ 10'000u };
		uint32_t commandsPerCallback{ 32u };
		uint32_t logMessagesPerWindow{ 100u };
		uint64_t logWindowTicks{ 600u };
	};

	enum class AgentBehaviourRuntimeFailure
	{
		None,
		LuaError,
		MemoryBudgetExceeded,
		InstructionBudgetExceeded,
		ConversionError
	};

	enum class AgentBehaviourRuntimeStage
	{
		ModuleLoad,
		Factory,
		Callback
	};

	// Defined lifetime endpoints passed to the best-effort on_stop callback.
	// They are runtime values only and are never part of Building persistence.
	enum class AgentBehaviourTeardownReason
	{
		Unassignment,
		Reset,
		Reload,
		BuildingClose,
		InstanceFailure,
		BehaviourDeletion
	};

	// Value-only failure record. Later failure-policy layers may decide how to
	// present or stop a run; the Lua boundary always records the original scope
	// and never lets a Lua/sol2 failure unwind through a simulation tick.
	struct AgentBehaviourRuntimeDiagnostic
	{
		AgentBehaviourRuntimeFailure failure{ AgentBehaviourRuntimeFailure::None };
		AgentBehaviourRuntimeStage stage{ AgentBehaviourRuntimeStage::Callback };
		AgentId agent{};
		AgentBehaviourId behaviour{};
		uint64_t tick{ 0 };
		std::string agentName;
		std::string behaviourName;
		std::string packageName;
		std::string moduleName;
		std::string callback;
		std::string diagnostic;
		std::string traceback;
	};

	// Ordinary C++ result returned by the Lua runtime boundary. Lua, sol2, and
	// their implementation types are deliberately confined to the adapter's
	// .cpp file and never enter authored-domain interfaces.
	struct AgentBehaviourModulePreflight
	{
		bool loaded{ false };
		AgentBehaviourRuntimeFailure failure{ AgentBehaviourRuntimeFailure::None };
		std::string diagnostic;
		std::string traceback;
	};

	// Immutable C++ source representation supplied to a scratch or private
	// per-Agent loader. The logical import name never derives a filesystem path.
	struct AgentBehaviourHelperSource
	{
		std::string name;
		std::string sourceModulePath;
		std::string source;
	};

	// One live adapter is owned by each Building. Its implementation owns that
	// Building's Lua state and private per-Agent module environments; Lua and sol2
	// remain confined to the .cpp file. Startup callbacks queue commands and the
	// adapter applies them through the Building facade only after every callback
	// at the boundary has returned.
	class AgentBehaviourRuntimeAdapter
	{
		friend class AgentBehaviourRegistry;
		friend class Building;

		struct Impl;
		std::unique_ptr<Impl> mImpl;

		// Builds every assigned instance in a fresh per-Building runtime without
		// running callbacks or touching the live Building. A successful candidate
		// can therefore be adopted only after all dependent Buildings preflight.
		static bool prepareReload(Building& building,
			AgentBehaviourRegistry const& registry,
			std::unique_ptr<AgentBehaviourRuntimeAdapter>& candidate,
			std::vector<AgentBehaviourRuntimeDiagnostic>& diagnostics,
			std::map<AgentId, AgentBehaviourAssignment> const* assignments = nullptr,
			AgentBehaviourId excludedBehaviour = {});
		void appendDiagnostics(
			std::vector<AgentBehaviourRuntimeDiagnostic> diagnostics);
		static AgentBehaviourModulePreflight preflightModule(
			std::string_view packageName, std::string_view moduleName,
			std::string_view source,
			std::vector<AgentBehaviourHelperSource> const& helpers,
			AgentBehaviourRuntimeLimits limits, bool invokeFactory);

	public:
		static constexpr uint32_t HostApiVersion{ 1 };
		static constexpr size_t DefaultMemoryBudgetBytes{ 64u * 1024u * 1024u };
		static constexpr uint32_t DefaultInstructionBudget{ 100'000u };
		static constexpr uint32_t DefaultTimersPerInstance{ 256u };
		static constexpr uint32_t DefaultCallbacksPerBoundary{ 10'000u };
		static constexpr uint32_t DefaultCommandsPerCallback{ 32u };
		static constexpr uint32_t DefaultLogMessagesPerWindow{ 100u };
		static constexpr uint64_t DefaultLogWindowTicks{ 600u };
		// Compatibility names for the scratch preflight API; scratch and live
		// runtimes intentionally use the same defaults.
		static constexpr size_t PreflightMemoryBudgetBytes{ DefaultMemoryBudgetBytes };
		static constexpr uint32_t PreflightInstructionBudget{ DefaultInstructionBudget };

		explicit AgentBehaviourRuntimeAdapter(
			AgentBehaviourRuntimeLimits limits = {});
		~AgentBehaviourRuntimeAdapter();
		AgentBehaviourRuntimeAdapter(AgentBehaviourRuntimeAdapter const&) = delete;
		AgentBehaviourRuntimeAdapter& operator=(AgentBehaviourRuntimeAdapter const&) = delete;

		// Called only with no active simulation phase. Missing instances are all
		// constructed first; startup and queued semantic outcomes run in stable
		// Agent/event order, then commands are applied before intent collection.
		// Returns false when this boundary diagnosed a failure. The Building is
		// paused before the caller can enter the next simulation tick.
		bool runBoundary(Building& building);
		void observeOutcome(SimulationEvent const& event);
		// Activation edits are paused-only. The transition is retained even when
		// the private instance will not be constructed until the next boundary.
		void observeActivation(SimulationEvent const& event);
		// Assignment edits are paused-only. Best-effort teardown cannot veto the
		// edit, and removing the private instance prevents stale outcome delivery.
		void removeInstance(Building& building, AgentId agent,
			AgentBehaviourTeardownReason reason =
				AgentBehaviourTeardownReason::Unassignment);
		void teardownAll(Building& building, AgentBehaviourTeardownReason reason);
		bool isInstanceDisabled(AgentId agent) const;
		std::vector<AgentBehaviourRuntimeDiagnostic> consumeDiagnostics();
		AgentBehaviourRuntimeLimits getLimits() const;
		void reset();

		static AgentBehaviourModulePreflight preflightModule(
			std::string_view packageName, std::string_view moduleName,
			std::string_view source,
			std::vector<AgentBehaviourHelperSource> const& helpers = {},
			AgentBehaviourRuntimeLimits limits = {});
		// Validates source, imports, and the API/module contract without inventing
		// configuration. Reload validates the factory separately with each real
		// affected Agent configuration.
		static AgentBehaviourModulePreflight preflightModuleContract(
			std::string_view packageName, std::string_view moduleName,
			std::string_view source,
			std::vector<AgentBehaviourHelperSource> const& helpers = {},
			AgentBehaviourRuntimeLimits limits = {});
		static AgentBehaviourModulePreflight preflightHelperModule(
			std::string_view packageName, std::string_view helperName,
			std::string_view moduleName, std::string_view source,
			std::vector<AgentBehaviourHelperSource> const& helpers,
			AgentBehaviourRuntimeLimits limits = {});
	};
}
