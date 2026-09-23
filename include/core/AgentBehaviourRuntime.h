#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace core
{
	// Ordinary C++ result returned by the Lua runtime boundary. Lua, sol2, and
	// their implementation types are deliberately confined to the adapter's
	// .cpp file and never enter authored-domain interfaces.
	struct AgentBehaviourModulePreflight
	{
		bool loaded{ false };
		std::string diagnostic;
		std::string traceback;
	};

	// Protected scratch-runtime adapter used to validate one behaviour module.
	// Preflight executes the module chunk and factory, but never invokes an
	// Agent instance callback. Each call owns a fresh state and fixed budgets.
	class AgentBehaviourRuntimeAdapter
	{
	public:
		static constexpr uint32_t HostApiVersion{ 1 };
		static constexpr size_t PreflightMemoryBudgetBytes{ 64u * 1024u * 1024u };
		static constexpr uint32_t PreflightInstructionBudget{ 100'000u };

		static AgentBehaviourModulePreflight preflightModule(
			std::string_view packageName, std::string_view moduleName,
			std::string_view source);
	};
}
