// Real Lua 5.4/sol2 module preflight checks for #150.

#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "core/AgentBehaviourRegistry.h"
#include "core/Building.h"
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

	std::string runStartupMovement(
		std::shared_ptr<core::AgentBehaviourRegistry> const& registry,
		core::AgentBehaviourId behaviour)
	{
		core::Building building("Lua startup", 14, 2);
		auto const room = building.addRoom("Room", 0, 0, 0, 14, 1);
		building.addSectorMarker(room, 0, 4.5f, "Near");
		building.addSectorMarker(room, 0, 11.5f, "Far");
		building.finishBuild();
		auto const first = building.createAgent("First", room, 0, 0.5f);
		auto const second = building.createAgent("Second", room, 0, 1.5f);
		auto const markers = building.getMarkerIds();

		building.pauseSimulation();
		building.consumeSimulationEvents();
		building.attachAgentBehaviourRegistry("startup.behaviours", registry);
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		require(building.setAgentBehaviourAssignment(first, behaviour, revision,
			{ { "destination", markers[0] } }),
			"Could not assign the first startup behaviour");
		require(building.setAgentBehaviourAssignment(second, behaviour, revision,
			{ { "destination", markers[1] } }),
			"Could not assign the second startup behaviour");
		require(building.resumeSimulation(), "Could not resume the Lua startup fixture");
		building.consumeSimulationEvents();

		building.advanceTick();
		auto firstTick = building.getSimulationSnapshot();
		require(firstTick.tick == 1 && firstTick.agents.size() == 2
			&& firstTick.agents[0].hasPath && firstTick.agents[1].hasPath
			&& firstTick.agents[0].globalPosition.x > 0.5f
			&& firstTick.agents[1].globalPosition.x > 1.5f,
			"on_start movement was not applied before first-tick intent and movement");

		std::ostringstream digest;
		unsigned reached = 0;
		auto observePublicEvents = [&]
		{
			for (auto const& event : building.consumeSimulationEvents())
			{
				if (event.type != core::SimulationEventType::DestinationReached) continue;
				++reached;
				digest << event.tick << ':' << event.agent.id.value << ':'
					<< event.destinationMarker.value << '|';
			}
		};
		observePublicEvents();
		for (unsigned tick = 0; tick < 1500 && reached < 2; ++tick)
		{
			building.advanceTick();
			observePublicEvents();
		}
		require(reached == 2,
			"Independently configured Lua Agents did not reach both Markers");

		auto const completed = building.getSimulationSnapshot();
		require(std::fabs(completed.agents[0].globalPosition.x - 4.5f) < 0.001f
			&& std::fabs(completed.agents[1].globalPosition.x - 11.5f) < 0.001f,
			"Shared behaviour instances did not retain distinct Marker configuration");
		building.advanceTicks(5);
		observePublicEvents();
		require(reached == 2, "on_start ran more than once for one instance lifetime");
		digest << completed.tick << ':'
			<< std::bit_cast<uint32_t>(completed.agents[0].globalPosition.x) << ':'
			<< std::bit_cast<uint32_t>(completed.agents[1].globalPosition.x);
		return digest.str();
	}

	void independentStartupInstancesMoveDeterministically()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "startup.behaviours";
		std::filesystem::create_directories(package);
		auto const manifest = package / "behaviours.yaml";
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo(manifest.string());
		writeText(package / "startup.lua", R"lua(
local host = require("prometheum.v1")
local factories_in_this_environment = 0
return {
  api_version = host.api_version,
  factory = function(configuration)
    factories_in_this_environment = factories_in_this_environment + 1
    if factories_in_this_environment ~= 1 then
      error("module environment was shared between Agents")
    end
    local instance = { starts = 0 }
    return {
      on_start = function(context, callback_configuration)
        instance.starts = instance.starts + 1
        if instance.starts ~= 1 then error("instance state was shared or restarted") end
        if callback_configuration ~= configuration
            or context.configuration ~= configuration then
          error("callback did not receive its immutable configuration")
        end
        if type(configuration.destination) ~= "userdata"
            or tonumber(configuration.destination) ~= nil then
          error("Marker was not an opaque handle")
        end
        if pcall(function() configuration.destination = false end) then
          error("configuration was mutable")
        end
        local result = context.move_to(configuration.destination)
        if not result.accepted or result.status ~= "accepted" then
          error("move_to did not return a semantic accepted result")
        end
        if pcall(function() result.status = "changed" end) then
          error("command result was mutable")
        end
        instance.retained_context = context
      end
    }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Startup", "startup.lua",
			{ { "destination", core::AgentBehaviourSchemaType::Marker } });
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The real startup Lua fixture did not preflight");

		auto const first = runStartupMovement(registry, behaviour);
		auto const second = runStartupMovement(registry, behaviour);
		require(first == second,
			"Per-Building Lua startup and movement were not deterministic");
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
	independentStartupInstancesMoveDeterministically();
	registryRetainsLoadedAndErrorStatus();
}
