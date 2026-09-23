#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <string_view>

namespace core
{
	class Building;
	struct SimulationEvent;
	// Ordinary C++ result returned by the Lua runtime boundary. Lua, sol2, and
	// their implementation types are deliberately confined to the adapter's
	// .cpp file and never enter authored-domain interfaces.
	struct AgentBehaviourModulePreflight
	{
		bool loaded{ false };
		std::string diagnostic;
		std::string traceback;
	};

	// One live adapter is owned by each Building. Its implementation owns that
	// Building's Lua state and private per-Agent module environments; Lua and sol2
	// remain confined to the .cpp file. Startup callbacks queue commands and the
	// adapter applies them through the Building facade only after every callback
	// at the boundary has returned.
	class AgentBehaviourRuntimeAdapter
	{
		struct Impl;
		std::unique_ptr<Impl> mImpl;

	public:
		static constexpr uint32_t HostApiVersion{ 1 };
		static constexpr size_t PreflightMemoryBudgetBytes{ 64u * 1024u * 1024u };
		static constexpr uint32_t PreflightInstructionBudget{ 100'000u };

		AgentBehaviourRuntimeAdapter();
		~AgentBehaviourRuntimeAdapter();
		AgentBehaviourRuntimeAdapter(AgentBehaviourRuntimeAdapter const&) = delete;
		AgentBehaviourRuntimeAdapter& operator=(AgentBehaviourRuntimeAdapter const&) = delete;

		// Called only with no active simulation phase. Missing instances are all
		// constructed first, then on_start runs once in ascending Agent ID, and
		// queued movement commands are applied before intent collection.
		void runStartupBoundary(Building& building);
		void observeOutcome(SimulationEvent const& event);
		void reset();

		static AgentBehaviourModulePreflight preflightModule(
			std::string_view packageName, std::string_view moduleName,
			std::string_view source);
	};
}
