// Real Lua 5.4/sol2 module preflight checks for #150.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRuntime.h"

void runAgentBehaviourRuntimeSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path;

		TemporaryDirectory()
		{
			path = std::filesystem::temp_directory_path()
				/ ("prometheum-fermide-lua-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}

		~TemporaryDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	void writeText(std::filesystem::path const& path, std::string const& source)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(source.data(), static_cast<std::streamsize>(source.size()));
		if (!output) throw std::runtime_error("Could not write Lua preflight fixture");
	}

	core::AgentBehaviourModulePreflight preflight(std::string const& source)
	{
		return core::AgentBehaviourRuntimeAdapter::preflightModule(
			"headless.behaviours", "schedule.lua", source);
	}

	void validHostContractDoesNotRunCallbacks()
	{
		auto result = preflight(R"lua(
local host = require("prometheum.v1")
if host.api_version ~= 1 then error("wrong host API") end
return {
  api_version = host.api_version,
  factory = function(configuration)
    return {
      on_start = function() error("preflight ran on_start") end,
      on_event = function() error("preflight ran on_event") end
    }
  end
}
)lua");
		require(result.loaded && result.diagnostic.empty() && result.traceback.empty(),
			"A valid version-1 behaviour did not preflight, or an Agent callback ran: "
				+ result.diagnostic);
	}

	void textAndContractFailuresCarryLocationAndTraceback()
	{
		auto syntax = preflight("return {\n  api_version = 1,\n  factory = function(\n}\n");
		require(!syntax.loaded
			&& syntax.diagnostic.find("headless.behaviours") != std::string::npos
			&& syntax.diagnostic.find("schedule.lua") != std::string::npos
			&& syntax.diagnostic.find("line 4") != std::string::npos
			&& !syntax.traceback.empty(),
			"Invalid text source lacked package/module/line diagnostics");

		std::string bytecode("\x1bLua", 4);
		bytecode.append("not source");
		auto compiled = preflight(bytecode);
		require(!compiled.loaded
			&& compiled.diagnostic.find("line 1") != std::string::npos
			&& compiled.diagnostic.find("bytecode") != std::string::npos,
			"Precompiled bytecode was not refused as non-text input");

		for (auto const& malformed : {
			std::string("return { factory = function() return {} end }"),
			std::string("return { api_version = 2, factory = function() return {} end }"),
			std::string("return { api_version = 1 }"),
			std::string("return { api_version = 1, factory = function() return false end }"),
			std::string("return { api_version = 1, factory = function() return { on_start = 4 } end }") })
		{
			auto result = preflight(malformed);
			require(!result.loaded && result.diagnostic.find("line 1") != std::string::npos,
				"A missing, mismatched, or malformed module contract was accepted");
		}

		auto factoryError = preflight(R"lua(
return {
  api_version = 1,
  factory = function()
    error("factory exploded")
  end
}
)lua");
		require(!factoryError.loaded
			&& factoryError.diagnostic.find("line 5") != std::string::npos
			&& factoryError.traceback.find("stack traceback") != std::string::npos
			&& factoryError.traceback.find("factory exploded") != std::string::npos,
			"A protected factory error lacked its source line and traceback");
	}

	void customLoaderIsReservedAndImmutable()
	{
		auto immutable = preflight(R"lua(
local host = require("prometheum.v1")
local changed = pcall(function() host.api_version = 2 end)
if changed or host.api_version ~= 1 then error("mutable host module") end
if package ~= nil then error("standard package library is enabled") end
return { api_version = 1, factory = function() return {} end }
)lua");
		require(immutable.loaded,
			"The reserved immutable host module was unavailable: " + immutable.diagnostic);

		auto undeclared = preflight(R"lua(
require("os")
return { api_version = 1, factory = function() return {} end }
)lua");
		require(!undeclared.loaded
			&& undeclared.diagnostic.find("line 2") != std::string::npos
			&& undeclared.traceback.find("not available") != std::string::npos,
			"The custom loader admitted a non-host module or omitted its traceback");
	}

	void scratchExecutionIsBudgeted()
	{
		auto runaway = preflight("while true do end\n");
		require(!runaway.loaded
			&& runaway.diagnostic.find("instruction budget") != std::string::npos,
			"A runaway module escaped the scratch-state instruction budget");

		auto excessiveAllocation = preflight(R"lua(
local excessive = string.rep("x", 70 * 1024 * 1024)
return { api_version = 1, factory = function() return {} end }
)lua");
		require(!excessiveAllocation.loaded
			&& excessiveAllocation.traceback.find("memory") != std::string::npos,
			"A module escaped the scratch-state memory budget");
	}

	void registryRetainsLoadedAndErrorStatus()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "status.behaviours";
		std::filesystem::create_directories(package);
		auto const manifest = package / "behaviours.yaml";
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo(manifest.string());
		writeText(package / "valid.lua",
			"local p=require('prometheum.v1'); return {api_version=p.api_version, factory=function() return {} end}\n");
		writeText(package / "broken.lua", "return { api_version = 1, factory = function( }\n");
		auto const valid = registry->addAgentBehaviour("Valid", "valid.lua", {});
		auto const broken = registry->addAgentBehaviour("Broken", "broken.lua", {});
		require(registry->lookupAgentBehaviour(valid)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded
			&& registry->lookupAgentBehaviour(broken)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Error
			&& !registry->lookupAgentBehaviour(broken)->getModuleTraceback().empty(),
			"The registry did not expose loaded/error module status and traceback");
		registry->saveTo(manifest.string());

		auto reopened = core::AgentBehaviourRegistry::loadFrom(manifest.string());
		require(reopened->lookupAgentBehaviour(valid)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded
			&& reopened->lookupAgentBehaviour(broken)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Error,
			"Real module preflight did not run when the registry reopened");
	}
}

void runAgentBehaviourRuntimeSmokeChecks()
{
	validHostContractDoesNotRunCallbacks();
	textAndContractFailuresCarryLocationAndTraceback();
	customLoaderIsReservedAndImmutable();
	scratchExecutionIsBudgeted();
	registryRetainsLoadedAndErrorStatus();
}
