// Real Lua 5.4/sol2 module preflight checks for #150.

#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
#include "core/Building.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/YamlSerializer.h"

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

	void prohibitedHostSurfacesAreAbsent()
	{
		auto result = preflight(R"lua(
local expected = {
  "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal",
  "rawget", "select", "tonumber", "tostring", "type", "xpcall",
  "table", "string", "math", "utf8", "require"
}
for _, name in ipairs(expected) do
  if _G[name] == nil then error("missing selected facility: " .. name) end
end
local prohibited = {
  "io", "os", "package", "debug", "coroutine", "load", "loadfile",
  "dofile", "collectgarbage", "getmetatable", "setmetatable", "rawset"
}
for _, name in ipairs(prohibited) do
  if _G[name] ~= nil then error("prohibited host surface: " .. name) end
end
if string.dump ~= nil then error("precompiled bytecode facility is available") end
if math.random ~= nil or math.randomseed ~= nil then
  error("nondeterministic entropy is available")
end
for _, module in ipairs({ "io", "os", "debug", "package", "coroutine",
    "socket", "lfs", "native.so" }) do
  if pcall(require, module) then error("loaded prohibited module: " .. module) end
end
return { api_version = 1, factory = function() return {} end }
)lua");
		require(result.loaded,
			"A prohibited host surface was visible, or a selected facility was absent: "
				+ result.diagnostic);
	}

	void customLoaderIsReservedAndImmutable()
	{
		auto immutable = preflight(R"lua(
local host = require("prometheum.v1")
local changed = pcall(function() host.api_version = 2 end)
if changed or host.api_version ~= 1 then error("mutable host module") end
if pcall(function() math.pi = 0 end)
    or pcall(function() string.byte = false end)
    or pcall(function() table.insert = false end)
    or pcall(function() utf8.char = false end) then
  error("mutable built-in library")
end
if package ~= nil then error("standard package library is enabled") end
return { api_version = 1, factory = function() return {} end }
)lua");
		require(immutable.loaded,
			"The reserved immutable host module was unavailable: " + immutable.diagnostic);
		std::string nameDiagnostic;
		require(core::AgentBehaviourHelperModule::nameIsValid(
			"helpers.values", &nameDiagnostic),
			"A dotted helper import name was refused");
		for (auto const& invalidName : { "", "prometheum.v1", "/absolute",
			"../traversal", "helpers/file", "native.dll", "helpers..value" })
			require(!core::AgentBehaviourHelperModule::nameIsValid(
				invalidName, &nameDiagnostic),
				"A path-like, reserved, or malformed helper import name was accepted");

		for (auto const& name : { "os", "/tmp/evil", "../evil", "native.dll",
			"helpers/../../evil" })
		{
			auto undeclared = core::AgentBehaviourRuntimeAdapter::preflightModule(
				"headless.behaviours", "schedule.lua",
				"require(\"" + std::string(name) + "\")\n"
				"return { api_version = 1, factory = function() return {} end }\n");
			require(!undeclared.loaded
				&& undeclared.traceback.find("not available") != std::string::npos,
				"The custom loader admitted an undeclared, path-based, or native module");
		}

		std::vector<core::AgentBehaviourHelperSource> helpers{
			{ "helpers.values", "modules/values.lua", R"lua(
local loads = 0
loads = loads + 1
return { loads = loads, nested = { answer = 42 } }
)lua" }
		};
		auto declared = core::AgentBehaviourRuntimeAdapter::preflightModule(
			"headless.behaviours", "schedule.lua", R"lua(
local first = require("helpers.values")
local second = require("helpers.values")
if first ~= second or first.loads ~= 1 or first.nested.answer ~= 42 then
  error("helper cache or exports were incorrect")
end
if pcall(function() first.loads = 2 end)
    or pcall(function() first.nested.answer = 0 end) then
  error("helper exports were mutable")
end
return { api_version = 1, factory = function() return {} end }
)lua", helpers);
		require(declared.loaded,
			"A declared helper graph was unavailable or mutable: " + declared.diagnostic);

		std::vector<core::AgentBehaviourHelperSource> cycle{
			{ "helpers.a", "modules/a.lua", "return require('helpers.b')\n" },
			{ "helpers.b", "modules/b.lua", "return require('helpers.a')\n" }
		};
		auto cyclic = core::AgentBehaviourRuntimeAdapter::preflightModule(
			"headless.behaviours", "schedule.lua", R"lua(
require("helpers.a")
return { api_version = 1, factory = function() return {} end }
)lua", cycle);
		require(!cyclic.loaded
			&& cyclic.traceback.find(
				"schedule.lua -> helpers.a -> helpers.b -> helpers.a")
				!= std::string::npos,
			"An import cycle was accepted or omitted its complete dependency chain");
	}

	void scratchExecutionIsBudgeted()
	{
		core::AgentBehaviourRuntimeLimits defaults;
		require(defaults.memoryBytes == 64u * 1024u * 1024u
			&& defaults.instructionsPerCall == 100'000u,
			"Lua containment defaults changed from 64 MiB/100,000 instructions");

		auto runaway = preflight("while true do end\n");
		require(!runaway.loaded
			&& runaway.failure
				== core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded
			&& runaway.diagnostic.find("instruction budget") != std::string::npos,
			"A runaway module escaped the scratch-state instruction budget");

		auto excessiveAllocation = preflight(R"lua(
local excessive = string.rep("x", 70 * 1024 * 1024)
return { api_version = 1, factory = function() return {} end }
)lua");
		require(!excessiveAllocation.loaded
			&& excessiveAllocation.failure
				== core::AgentBehaviourRuntimeFailure::MemoryBudgetExceeded
			&& excessiveAllocation.traceback.find("memory") != std::string::npos,
			"A module escaped the scratch-state memory budget");

		auto configured = core::AgentBehaviourRuntimeAdapter::preflightModule(
			"headless.behaviours", "configured.lua", R"lua(
local total = 0
for i = 1, 1000 do total = total + i end
return { api_version = 1, factory = function() return {} end }
)lua", {}, { 2u * 1024u * 1024u, 100u });
		require(!configured.loaded
			&& configured.failure
				== core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded,
			"The application-configured scratch instruction budget was ignored");

		auto recovered = preflight(
			"return { api_version = 1, factory = function() return {} end }\n");
		require(recovered.loaded,
			"A refused scratch allocation corrupted later Lua state creation");
	}

	void liveLoadsFactoriesAndCallbacksAreContained()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "abuse.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		auto add = [&](std::string const& name, std::string const& filename,
			std::string const& source)
		{
			writeText(package / filename, source);
			auto const id = registry->addAgentBehaviour(name, filename, {});
			require(registry->lookupAgentBehaviour(id)->getModuleStatus()
					== core::AgentBehaviourModuleStatus::Loaded,
				"An abuse fixture failed ordinary protected preflight: " + name);
			return id;
		};
		auto const loadBudget = add("Load budget", "load-budget.lua", R"lua(
local total = 0
for i = 1, 5000 do total = total + i end
return { api_version = 1, factory = function() return {} end }
)lua");
		auto const factoryBudget = add("Factory budget", "factory-budget.lua", R"lua(
return { api_version = 1, factory = function()
  local total = 0
  for i = 1, 5000 do total = total + i end
  return {}
end }
)lua");
		auto const callbackBudget = add("Callback budget", "callback-budget.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function() while true do end end }
end }
)lua");
		auto const memoryBudget = add("Memory budget", "memory-budget.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function()
    local excessive = string.rep("x", 70 * 1024 * 1024)
    if #excessive == 0 then error("unreachable") end
  end }
end }
)lua");
		auto const luaError = add("Lua error", "lua-error.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function() error("contained callback error") end }
end }
)lua");
		auto const safe = add("Safe", "safe.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context)
    local result = context.cancel_movement()
    if not result.accepted or result.status ~= "no_op" then error(result.status) end
  end }
end }
)lua");

		auto runInstructionStage = [&](core::AgentBehaviourId behaviour,
			core::AgentBehaviourRuntimeStage expectedStage)
		{
			core::Building building("Instruction containment", 6, 2,
				{ 64u * 1024u * 1024u, 1'000u });
			auto const room = building.addRoom("Room", 0, 0, 0, 6, 1);
			building.finishBuild();
			auto const agent = building.createAgent("Abusive", room, 0, 0.5f);
			building.pauseSimulation();
			building.attachAgentBehaviourRegistry("abuse.behaviours", registry);
			require(building.setAgentBehaviourAssignment(agent, behaviour,
				registry->lookupAgentBehaviour(behaviour)->getRevision(), {}),
				"Could not assign an instruction abuse fixture");
			require(building.getAgentBehaviourRuntimeLimits().instructionsPerCall == 1'000u,
				"The per-Building instruction limit was not retained");
			require(building.resumeSimulation(),
				"Could not resume an instruction abuse fixture");
			building.advanceTick();
			auto diagnostics = building.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1
				&& diagnostics[0].failure
					== core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded
				&& diagnostics[0].stage == expectedStage
				&& diagnostics[0].agent == agent
				&& !building.agentBehaviourOwnsMovement(agent),
				"Instruction exhaustion escaped, lacked structure, or retained ownership");
		};
		runInstructionStage(loadBudget,
			core::AgentBehaviourRuntimeStage::ModuleLoad);
		runInstructionStage(factoryBudget,
			core::AgentBehaviourRuntimeStage::Factory);
		runInstructionStage(callbackBudget,
			core::AgentBehaviourRuntimeStage::Callback);

		{
			core::Building building("Memory recovery", 8, 2,
				{ 2u * 1024u * 1024u, 100'000u });
			auto const room = building.addRoom("Room", 0, 0, 0, 8, 1);
			building.finishBuild();
			auto const abusive = building.createAgent("Abusive", room, 0, 0.5f);
			auto const healthy = building.createAgent("Healthy", room, 0, 1.5f);
			building.pauseSimulation();
			building.attachAgentBehaviourRegistry("abuse.behaviours", registry);
			require(building.getAgentBehaviourRuntimeLimits().memoryBytes
					== 2u * 1024u * 1024u,
				"The per-Building heap limit was not retained");
			require(building.setAgentBehaviourAssignment(abusive, memoryBudget,
				registry->lookupAgentBehaviour(memoryBudget)->getRevision(), {})
				&& building.setAgentBehaviourAssignment(healthy, safe,
					registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not assign the live allocator recovery fixtures");
			// The healthy callback exercises a host capability after the refused
			// allocation while the failed instance releases its Lua heap graph.
			require(building.resumeSimulation(),
				"Could not resume the live allocator fixture");
			building.advanceTick();
			auto diagnostics = building.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1
				&& diagnostics[0].failure
					== core::AgentBehaviourRuntimeFailure::MemoryBudgetExceeded
				&& diagnostics[0].stage == core::AgentBehaviourRuntimeStage::Callback
				&& diagnostics[0].callback == "on_start"
				&& diagnostics[0].agent == abusive
				&& !building.agentBehaviourOwnsMovement(abusive)
				&& building.agentBehaviourOwnsMovement(healthy),
				"Live heap exhaustion corrupted the state or disabled a healthy instance");

			building.pauseSimulation();
			require(building.clearAgentBehaviourAssignment(abusive),
				"Could not remove the exhausted instance during recovery");
			require(building.setAgentBehaviourAssignment(abusive, safe,
				registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not create a replacement after refused allocation");
			require(building.resumeSimulation(),
				"Could not resume after refused allocation");
			building.advanceTick();
			require(building.consumeAgentBehaviourRuntimeDiagnostics().empty()
				&& building.agentBehaviourOwnsMovement(abusive),
				"The Building Lua state did not recover after a refused allocation");
		}

		{
			core::Building building("Lua error containment", 6, 2);
			auto const room = building.addRoom("Room", 0, 0, 0, 6, 1);
			building.finishBuild();
			auto const agent = building.createAgent("Abusive", room, 0, 0.5f);
			building.pauseSimulation();
			building.attachAgentBehaviourRegistry("abuse.behaviours", registry);
			require(building.setAgentBehaviourAssignment(agent, luaError,
				registry->lookupAgentBehaviour(luaError)->getRevision(), {}),
				"Could not assign the protected Lua error fixture");
			require(building.resumeSimulation(),
				"Could not resume the protected Lua error fixture");
			building.advanceTick();
			auto diagnostics = building.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1
				&& diagnostics[0].failure == core::AgentBehaviourRuntimeFailure::LuaError
				&& diagnostics[0].traceback.find("contained callback error")
					!= std::string::npos,
				"A Lua error unwound through the tick or lacked a structured diagnostic");
		}
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
		require(building.agentBehaviourOwnsMovement(first)
			&& building.agentBehaviourOwnsMovement(second),
			"Assigned enabled behaviours did not acquire movement ownership");
		require(building.moveAgentToMarker(first, markers[0]).status
				== core::MovementCommandStatus::BehaviourOwned
			&& building.cancelAgentMovement(first).status
				== core::MovementCommandStatus::BehaviourOwned,
			"Manual movement commands bypassed behaviour ownership");
		auto firstAgent = building.lookupAgent(first).entity;
		auto manualTarget = building.getGraph()->getClosestVertexInSector(
			firstAgent->getSector(), { 3.5f, 0.0f });
		firstAgent->setPath(building.getGraph()->calculatePath(firstAgent, manualTarget), true);
		require(!firstAgent->getPath(),
			"Direct manual Path assignment bypassed behaviour ownership");
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
				digest << event.tick << ':' << event.sequence << ':'
					<< event.agent.id.value << ':' << event.destinationMarker.value << '|';
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
		require(reached == 2, "on_start or destination_reached delivery ran more than once");
		require(building.agentBehaviourOwnsMovement(first)
			&& building.agentBehaviourOwnsMovement(second),
			"A valid immutable destination_reached callback disabled its instance");
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
      end,
      on_event = function(event, context)
        instance.events = (instance.events or 0) + 1
        if instance.events ~= 1
            or event.type ~= "destination_reached"
            or type(event.tick) ~= "number"
            or type(event.sequence) ~= "number"
            or event.destination ~= configuration.destination then
          error("destination_reached payload was missing, mutable, or duplicated")
        end
        if pcall(function() event.type = "changed" end) then
          error("semantic movement event was mutable")
        end
        local cancellation = context.cancel_movement()
        if not cancellation.accepted or cancellation.status ~= "no_op" then
          error("idle cancellation did not return semantic no_op")
        end
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

	void manifestHelpersHavePrivatePerAgentGraphs()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "helpers.behaviours";
		std::filesystem::create_directories(package);
		auto const manifest = package / "behaviours.yaml";
		auto const uuid = std::string("123e4567-e89b-42d3-a456-426614174156");
		auto const helperSource = R"lua(
local calls = 0
return {
  nested = { value = 7 },
  increment = function()
    calls = calls + 1
    return calls
  end
}
)lua";
		writeText(package / "counter.lua", helperSource);
		writeText(package / "private.lua", R"lua(
local host = require("prometheum.v1")
local counter = require("helpers.counter")
local cached = require("helpers.counter")
if counter ~= cached then error("helper cache was not instance-local") end
if pcall(function() counter.increment = false end)
    or pcall(function() counter.nested.value = 0 end) then
  error("helper exports were mutable")
end
return {
  api_version = host.api_version,
  factory = function(configuration)
    if counter.increment() ~= 1 then
      error("helper upvalues leaked between Agent instances")
    end
    return {
      on_start = function(context)
        local result = context.move_to(configuration.destination)
        if not result.accepted then error(result.status) end
      end
    }
  end
}
)lua");
		auto manifestText = [&](uint64_t revision, std::string const& helperPath)
		{
			return ""
				"  version: 1\n"
				"  uuid: " + uuid + "\n"
				"  revision: " + std::to_string(revision) + "\n"
				"  modules:\n"
				"    - name: helpers.counter\n"
				"      source: " + helperPath + "\n"
				"  nextBehaviourId: 2\n"
				"  behaviours:\n"
				"    - id: 1\n"
				"      name: Private helpers\n"
				"      revision: 1\n"
				"      source: private.lua\n"
				"      schema:\n"
				"        - name: destination\n"
				"          type: marker\n";
		};
		writeText(manifest, manifestText(1, "counter.lua"));
		auto registry = core::AgentBehaviourRegistry::loadFrom(manifest.string());
		auto const behaviour = core::AgentBehaviourId{ 1 };
		auto const* helper = registry->lookupHelperModule("helpers.counter");
		require(registry->getPackageRevision() == 1 && helper
			&& helper->getModuleStatus() == core::AgentBehaviourModuleStatus::Loaded
			&& registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"Manifest helper declarations did not participate in package status");

		// Runtime construction consumes the text admitted by preflight, never an
		// un-reloaded filesystem edit. Every Agent still executes that cached graph.
		writeText(package / "counter.lua", "error('unpreflighted edit executed')\n");
		auto const first = runStartupMovement(registry, behaviour);
		auto const second = runStartupMovement(registry, behaviour);
		require(first == second,
			"Private helper graphs were not deterministic across Building runtimes");

		// Changing the declared dependency set is a registry revision change, not
		// an invisible path substitution.
		writeText(package / "counter-v2.lua", helperSource);
		writeText(manifest, manifestText(1, "counter-v2.lua"));
		auto sameRevision = core::AgentBehaviourRegistry::loadFrom(manifest.string());
		std::string diagnostic;
		require(!registry->replaceDefinitionsFrom(std::move(*sameRevision), &diagnostic)
			&& diagnostic.find("package revision") != std::string::npos,
			"A helper dependency changed without advancing the package revision");
		writeText(manifest, manifestText(2, "counter-v2.lua"));
		auto advanced = core::AgentBehaviourRegistry::loadFrom(manifest.string());
		require(registry->replaceDefinitionsFrom(std::move(*advanced), &diagnostic)
			&& registry->getPackageRevision() == 2
			&& registry->lookupHelperModule("helpers.counter")->getSourceModulePath()
				== "counter-v2.lua",
			"An advanced helper dependency revision was not adopted deterministically");
	}

	void routeLossAndTopologyLifecycle()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "lifecycle.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "lifecycle.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local losses = 0
    return {
      on_start = function(context)
        local result = context.move_to(configuration.destination)
        if not result.accepted or result.status ~= "accepted" then error(result.status) end
      end,
      on_route_lost = function(destination, reason, context)
        losses = losses + 1
        if losses ~= 1 or destination ~= configuration.destination
            or reason ~= configuration.expected_reason then
          error("incorrect or duplicate route-loss callback")
        end
        local result = context.move_to(configuration.fallback)
        if not result.accepted or result.status ~= "accepted" then error(result.status) end
      end
    }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Lifecycle", "lifecycle.lua", {
			{ "destination", core::AgentBehaviourSchemaType::Marker },
			{ "fallback", core::AgentBehaviourSchemaType::Marker },
			{ "expected_reason", core::AgentBehaviourSchemaType::String }
		});
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The route-loss lifecycle fixture did not preflight");

		auto runTopology = [&](bool disconnect)
		{
			core::Building building(disconnect ? "Lost route" : "Replacement route", 8, 2);
			auto const front = building.addRoom("Front", 0, 0, 0, 8, 1);
			auto const back = building.addRoom("Back", 1, 0, 0, 8, 1);
			auto const door = building.addSectorDoor(front, 0, 2, {});
			building.addSectorMarker(back, 0, 6.5f, "Destination");
			building.addSectorMarker(front, 0, 0.5f, "Fallback");
			building.finishBuild();
			auto const markers = building.getMarkerIds();
			auto const id = building.createAgent("Walker", front, 0, 0.5f);
			building.pauseSimulation();
			building.attachAgentBehaviourRegistry("lifecycle.behaviours", registry);
			auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
			require(building.setAgentBehaviourAssignment(id, behaviour, revision, {
				{ "destination", markers[0] }, { "fallback", markers[1] },
				{ "expected_reason", std::string("topology_changed") }
			}), "Could not assign topology lifecycle behaviour");
			require(building.resumeSimulation(), "Could not start topology lifecycle fixture");
			building.consumeSimulationEvents();
			building.advanceTicks(5);
			building.consumeSimulationEvents();
			building.pauseSimulation();
			if (disconnect)
				require(building.removeSectorDoor(front, door.door.index),
					"Could not remove the topology fixture Door");
			building.finishBuild();
			require(building.resumeSimulation(), "Could not resume rebuilt topology fixture");
			building.consumeSimulationEvents();

			unsigned losses = 0, reached = 0;
			core::MarkerId reachedMarker;
			for (unsigned tick = 0; tick < 3000 && reached == 0; ++tick)
			{
				building.advanceTick();
				for (auto const& event : building.consumeSimulationEvents())
				{
					if (event.type == core::SimulationEventType::RouteLost)
					{
						++losses;
						require(event.destinationMarker == markers[0]
							&& event.routeLossReason == core::RouteLossReason::TopologyChanged,
							"Topology route loss lacked its destination or semantic reason");
					}
					if (event.type == core::SimulationEventType::DestinationReached)
					{
						++reached;
						reachedMarker = event.destinationMarker;
					}
				}
			}
			require(reached == 1 && losses == (disconnect ? 1u : 0u)
				&& reachedMarker == markers[disconnect ? 1u : 0u],
				std::string(disconnect
					? "Failed topology restoration did not call on_route_lost exactly once"
					: "Valid same-destination topology replanning called Lua or lost its goal")
					+ " (losses=" + std::to_string(losses)
					+ ", reached=" + std::to_string(reached)
					+ ", marker=" + std::to_string(reachedMarker.value) + ")");
		};
		runTopology(false);
		runTopology(true);

		core::Building unreachable("Initial route loss", 12, 2);
		auto const origin = unreachable.addRoom("Origin", 0, 0, 0, 6, 1);
		auto const isolated = unreachable.addRoom("Isolated", 1, 0, 6, 6, 1);
		unreachable.addSectorMarker(isolated, 0, 3.5f, "Destination");
		unreachable.addSectorMarker(origin, 0, 4.5f, "Fallback");
		unreachable.finishBuild();
		auto const markers = unreachable.getMarkerIds();
		auto const id = unreachable.createAgent("Walker", origin, 0, 0.5f);
		unreachable.pauseSimulation();
		unreachable.attachAgentBehaviourRegistry("lifecycle.behaviours", registry);
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		require(unreachable.setAgentBehaviourAssignment(id, behaviour, revision, {
			{ "destination", markers[0] }, { "fallback", markers[1] },
			{ "expected_reason", std::string("unreachable") }
		}), "Could not assign initial route-loss behaviour");
		require(unreachable.resumeSimulation(), "Could not start initial route-loss fixture");
		unreachable.consumeSimulationEvents();
		unsigned losses = 0, reached = 0;
		for (unsigned tick = 0; tick < 1500 && reached == 0; ++tick)
		{
			unreachable.advanceTick();
			for (auto const& event : unreachable.consumeSimulationEvents())
			{
				if (event.type == core::SimulationEventType::RouteLost)
				{
					++losses;
					require(event.destinationMarker == markers[0]
						&& event.routeLossReason == core::RouteLossReason::Unreachable,
						"Initial route loss lacked its destination or unreachable reason");
				}
				if (event.type == core::SimulationEventType::DestinationReached) ++reached;
			}
		}
		require(losses == 1 && reached == 1,
			"Initial unreachability did not clear the goal before one fallback command");
	}

	void programmingErrorDisablesMovementOwnership()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "programming-error.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "duplicate.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    return { on_start = function(context)
      context.move_to(configuration.destination)
      context.cancel_movement()
    end }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Duplicate", "duplicate.lua",
			{ { "destination", core::AgentBehaviourSchemaType::Marker } });
		writeText(package / "moving.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    return { on_start = function(context)
      local result = context.move_to(configuration.destination)
      if not result.accepted then error(result.status) end
    end }
  end
}
)lua");
		auto const movingBehaviour = registry->addAgentBehaviour("Moving", "moving.lua",
			{ { "destination", core::AgentBehaviourSchemaType::Marker } });
		core::Building building("Programming error", 8, 2);
		auto const room = building.addRoom("Room", 0, 0, 0, 8, 1);
		building.addSectorMarker(room, 0, 6.5f, "Destination");
		building.addSectorMarker(room, 0, 0.5f, "Return");
		building.finishBuild();
		auto const markers = building.getMarkerIds();
		auto const marker = markers[0];
		auto const id = building.createAgent("Walker", room, 0, 0.5f);
		building.pauseSimulation();
		building.attachAgentBehaviourRegistry("programming-error.behaviours", registry);
		require(building.setAgentBehaviourAssignment(id, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			{ { "destination", marker } }), "Could not assign duplicate-command fixture");
		require(building.resumeSimulation(), "Could not start duplicate-command fixture");
		building.consumeSimulationEvents();
		building.advanceTick();
		require(!building.agentBehaviourOwnsMovement(id)
			&& !building.lookupAgent(id).entity->getPath(),
			"A multiple-movement-command callback partially applied or retained ownership");
		require(building.moveAgentToMarker(id, marker).accepted(),
			"Disabling the failed instance did not restore manual movement controls");
		building.advanceTicks(1000);
		unsigned reached = 0;
		for (auto const& event : building.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::DestinationReached) ++reached;
		require(reached == 1, "Manual movement did not work after instance disablement");

		building.pauseSimulation();
		require(building.clearAgentBehaviourAssignment(id),
			"Could not unassign the disabled behaviour");
		require(!building.agentBehaviourOwnsMovement(id),
			"Unassignment left runtime movement ownership behind");

		require(building.setAgentBehaviourAssignment(id, movingBehaviour,
			registry->lookupAgentBehaviour(movingBehaviour)->getRevision(),
			{ { "destination", markers[1] } }),
			"Could not assign the active-unassignment fixture");
		require(building.resumeSimulation(), "Could not start active-unassignment fixture");
		building.advanceTick();
		require(building.lookupAgent(id).entity->getPath()
			&& building.agentBehaviourOwnsMovement(id),
			"The active-unassignment fixture did not acquire a route");
		building.pauseSimulation();
		require(building.clearAgentBehaviourAssignment(id),
			"Could not unassign an actively moving behaviour");
		require(!building.agentBehaviourOwnsMovement(id)
			&& !building.lookupAgent(id).entity->getPath(),
			"Active unassignment retained runtime movement ownership");
		require(building.resumeSimulation(), "Could not resume after active unassignment");
		require(building.moveAgentToMarker(id, markers[1]).accepted(),
			"Active unassignment did not restore manual movement commands");
	}

	std::string runDeterministicTimersAndSemanticState()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "timers.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "timers.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local fired = {}
    local function check_state(context, moving)
      local state = context.agent
      if state ~= context.state or type(state.identity) ~= "userdata"
          or state.name ~= configuration.expected_name
          or state.active ~= true or state.suspended ~= false
          or state.status ~= "active" or state.sector_name ~= "Room"
          or type(state.sector) ~= "userdata"
          or state.sector_identity ~= state.sector
          or state.sector_display_name ~= state.sector_name
          or type(state.global_position.x) ~= "number"
          or type(state.global_position.y) ~= "number"
          or state.position ~= state.global_position
          or state.tick ~= context.tick or state.simulation_tick ~= context.tick then
        error("semantic Agent state is incomplete")
      end
      if moving then
        if state.movement_state ~= "moving" or state.movement ~= "moving"
            or state.destination ~= configuration.destination
            or state.destination_marker ~= configuration.destination then
          error("semantic movement state or destination is incorrect")
        end
      elseif state.movement_state ~= "idle" or state.destination ~= nil then
        error("initial semantic movement state is incorrect")
      end
      for _, prohibited in ipairs({ "path", "vertex", "traversal_resource",
          "request", "permit", "queue", "snapshot", "userdata" }) do
        if state[prohibited] ~= nil then error("exposed " .. prohibited) end
      end
      if pcall(function() state.name = "changed" end)
          or pcall(function() state.global_position.x = 0 end)
          or pcall(function() state.sector.value = 1 end) then
        error("semantic Agent state was mutable")
      end
    end
    return {
      on_start = function(context)
        check_state(context, false)
        if configuration.overflow then
          context.set_timer("one", 1)
          context.set_timer("two", 1)
          context.set_timer("three", 1)
          return
        end
        context.set_timer("cancelled", 1)
        local cancelled = context.cancel_timer("cancelled")
        local replaced = context.set_timer("z", 3)
        local first = context.set_timer("a", 1)
        local replacement = context.set_timer("z", 1)
        local missing = context.cancel_timer("missing")
        if replaced.status ~= "accepted" or first.status ~= "accepted"
            or replacement.status ~= "accepted" or missing.status ~= "no_op"
            or cancelled.status ~= "accepted" then
          error("timer command result was incorrect")
        end
      end,
      on_timer = function(name, context)
        fired[#fired + 1] = name .. ":" .. context.tick
        if #fired == 1 then
          if fired[1] ~= "a:1" then error("first timer was not lexical a:1") end
        elseif #fired == 2 then
          if fired[2] ~= "z:1" then error("replacement or lexical order failed") end
          context.set_timer("finish", 1)
        elseif #fired == 3 then
          if fired[3] ~= "finish:2" then error("one-tick timer did not fire after tick N+1") end
          local moved = context.move_to(configuration.destination)
          if moved.status ~= "accepted" then error(moved.status) end
          context.set_timer("inspect", 1)
        elseif #fired == 4 then
          if fired[4] ~= "inspect:3" then error("timer callback sequence changed") end
          check_state(context, true)
        else
          error("one-shot timer fired more than once")
        end
      end
    }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Timers", "timers.lua", {
			{ "expected_name", core::AgentBehaviourSchemaType::String },
			{ "destination", core::AgentBehaviourSchemaType::Marker },
			{ "overflow", core::AgentBehaviourSchemaType::Boolean }
		});
		writeText(package / "timer-order.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function()
    local trace = ""
    return {
      on_start = function(context)
        context.set_timer("b", 1)
        context.set_timer("a", 1)
      end,
      on_timer = function(name)
        trace = trace .. name
        if name == "b" then
          if trace ~= "ab" then error("nonlexical:" .. trace) end
          error("ordered:" .. trace)
        end
      end
    }
  end
}
)lua");
		auto const orderingBehaviour = registry->addAgentBehaviour(
			"Timer order", "timer-order.lua", {});
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded
			&& registry->lookupAgentBehaviour(orderingBehaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The deterministic timer fixtures did not preflight");

		core::Building building("Timers", 16, 2,
			{ 64u * 1024u * 1024u, 100'000u, 2u });
		auto const room = building.addRoom("Room", 0, 0, 0, 16, 1);
		building.addSectorMarker(room, 0, 13.5f, "Destination");
		building.finishBuild();
		auto const overflow = building.createAgent("Overflow", room, 0, 0.5f);
		auto const first = building.createAgent("First", room, 0, 1.5f);
		auto const second = building.createAgent("Second", room, 0, 2.5f);
		auto const orderFirst = building.createAgent("Order first", room, 0, 3.5f);
		auto const orderSecond = building.createAgent("Order second", room, 0, 4.5f);
		auto const destination = building.getMarkerIds().front();
		building.pauseSimulation();
		building.consumeSimulationEvents();
		building.attachAgentBehaviourRegistry("timers.behaviours", registry);
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		auto assign = [&](core::AgentId id, std::string name, bool exceedsLimit)
		{
			require(building.setAgentBehaviourAssignment(id, behaviour, revision, {
				{ "expected_name", std::move(name) }, { "destination", destination },
				{ "overflow", exceedsLimit }
			}), "Could not assign deterministic timer fixture");
		};
		assign(overflow, "Overflow", true);
		assign(first, "First", false);
		assign(second, "Second", false);
		auto const orderingRevision = registry->lookupAgentBehaviour(
			orderingBehaviour)->getRevision();
		require(building.setAgentBehaviourAssignment(orderFirst, orderingBehaviour,
				orderingRevision, {})
			&& building.setAgentBehaviourAssignment(orderSecond, orderingBehaviour,
				orderingRevision, {}),
			"Could not assign callback-order timer fixtures");
		require(building.getAgentBehaviourRuntimeLimits().timersPerInstance == 2,
			"The configured per-instance timer limit was not retained");
		require(building.resumeSimulation(), "Could not resume timer fixture");
		building.consumeSimulationEvents();

		std::ostringstream digest;
		unsigned phaseEvents = 0;
		auto consume = [&]
		{
			for (auto const& event : building.consumeSimulationEvents())
			{
				if (event.type == core::SimulationEventType::PhaseCompleted) ++phaseEvents;
				if (event.type == core::SimulationEventType::AgentChanged
					|| event.type == core::SimulationEventType::DestinationReached)
					digest << event.tick << ':' << event.sequence << ':'
						<< static_cast<unsigned>(event.type) << ':'
						<< event.agent.id.value << '|';
			}
		};
		for (unsigned tick = 0; tick < 4; ++tick)
		{
			building.advanceTick();
			consume();
		}
		auto diagnostics = building.consumeAgentBehaviourRuntimeDiagnostics();
		require(diagnostics.size() == 3 && diagnostics[0].agent == overflow
			&& diagnostics[0].callback == "on_start"
			&& diagnostics[0].diagnostic.find("timer limit of 2") != std::string::npos
			&& diagnostics[1].agent == orderFirst
			&& diagnostics[1].callback == "on_timer"
			&& diagnostics[1].diagnostic.find("ordered:ab") != std::string::npos
			&& diagnostics[2].agent == orderSecond
			&& diagnostics[2].callback == "on_timer"
			&& diagnostics[2].diagnostic.find("ordered:ab") != std::string::npos,
			"Timer names were not lexical, callbacks were not in Agent-ID order, or the timer limit had the wrong scope");
		require(!building.agentBehaviourOwnsMovement(overflow)
			&& building.agentBehaviourOwnsMovement(first)
			&& building.agentBehaviourOwnsMovement(second),
			"One instance's timer limit affected another instance");
		require(building.lookupAgent(first).entity->getPath()
			&& building.lookupAgent(second).entity->getPath(),
			"Lexically ordered one-shot timers did not apply their movement commands");

		unsigned reached = 0;
		for (unsigned tick = 0; tick < 2000 && reached < 2; ++tick)
		{
			building.advanceTick();
			for (auto const& event : building.consumeSimulationEvents())
			{
				if (event.type == core::SimulationEventType::PhaseCompleted) ++phaseEvents;
				if (event.type != core::SimulationEventType::DestinationReached) continue;
				++reached;
				digest << event.tick << ':' << event.sequence << ":reached:"
					<< event.agent.id.value << '|';
			}
		}
		require(reached == 2 && phaseEvents != 0,
			"Timer-driven Agents did not finish, or Lua consumed the public event queue");
		building.advanceTicks(5);
		consume();
		require(building.consumeAgentBehaviourRuntimeDiagnostics().empty(),
			"A one-shot timer repeated or semantic state changed unexpectedly");
		return digest.str();
	}

	void deterministicTimersExposeOnlySemanticState()
	{
		auto const first = runDeterministicTimersAndSemanticState();
		auto const second = runDeterministicTimersAndSemanticState();
		require(first == second,
			"Timer callbacks or independently consumable public events were nondeterministic");
	}

	void activationSuspendsStateAndFreezesTimers()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "activation.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "activation.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local starts = 0
    local private_state = "secret_instance_155"
    local deactivations = 0
    local activations = 0
    return {
      on_start = function(context)
        starts = starts + 1
        if starts ~= 1 then error("on_start repeated") end
        context.set_timer("frozen_timer_155", 3)
      end,
      on_event = function(event, context)
        if event.type == "deactivated" then
          deactivations = deactivations + 1
          private_state = private_state .. ":suspended"
          if deactivations ~= 1 or event.destination ~= nil
              or context.agent.active or not context.agent.suspended then
            error("deactivation was missing or duplicated")
          end
          local command = context.move_to(configuration.destination)
          if command.accepted or command.status ~= "inactive_agent" then
            error("deactivated callback issued a command")
          end
        elseif event.type == "activated" then
          activations = activations + 1
          if activations ~= 1 or starts ~= 1
              or private_state ~= "secret_instance_155:suspended"
              or not context.agent.active or context.agent.suspended then
            error("reactivation replaced private state or reran on_start")
          end
        else
          error("unexpected lifecycle event " .. event.type)
        end
        if type(event.tick) ~= "number" or type(event.sequence) ~= "number"
            or pcall(function() event.type = "changed" end) then
          error("mutable lifecycle event")
        end
      end,
      on_timer = function(name, context)
        if name ~= "frozen_timer_155" or starts ~= 1 or activations ~= 1 then
          error("timer state was not preserved")
        end
        local moved = context.move_to(configuration.destination)
        if not moved.accepted then error(moved.status) end
      end
    }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Activation",
			"activation.lua", {
				{ "destination", core::AgentBehaviourSchemaType::Marker }
			});

		core::Building building("Activation lifetime", 12, 2);
		auto const room = building.addRoom("Room", 0, 0, 0, 12, 1);
		building.addSectorMarker(room, 0, 10.5f, "Destination");
		building.finishBuild();
		auto const agent = building.createAgent("Sleeper", room, 0, 0.5f);
		auto const destination = building.getMarkerIds().front();
		building.pauseSimulation();
		building.attachAgentBehaviourRegistry("activation.behaviours", registry);
		require(building.setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			{ { "destination", destination } }),
			"Could not assign activation lifetime fixture");
		require(building.resumeSimulation(), "Could not start activation fixture");
		building.consumeSimulationEvents();
		building.advanceTick(); // on_start at tick 0; timer due at tick 3.
		building.pauseSimulation();
		building.consumeSimulationEvents();
		require(building.setAgentActive(agent, false), "Could not deactivate Agent");
		require(building.setAgentActive(agent, false),
			"Idempotent deactivation was refused");
		unsigned deactivatedEvents = 0;
		for (auto const& event : building.consumeSimulationEvents())
			deactivatedEvents += event.type
				== core::SimulationEventType::AgentDeactivated;
		require(deactivatedEvents == 1,
			"Deactivation did not publish exactly one semantic transition");

		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		building.serialize(*writer, work);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("secret_instance_155") == std::string::npos
			&& yaml.find("frozen_timer_155") == std::string::npos,
			"Private instance state or timers entered Building persistence");

		require(building.resumeSimulation(), "Could not run deactivated fixture");
		building.advanceTicks(5);
		require(!building.lookupAgent(agent).entity->getPath(),
			"A suspended timer or command moved a deactivated Agent");
		building.pauseSimulation();
		building.consumeSimulationEvents();
		require(building.setAgentActive(agent, true), "Could not reactivate Agent");
		unsigned activatedEvents = 0;
		for (auto const& event : building.consumeSimulationEvents())
			activatedEvents += event.type == core::SimulationEventType::AgentActivated;
		require(activatedEvents == 1,
			"Reactivation did not publish exactly one semantic transition");
		require(building.resumeSimulation(), "Could not resume reactivated fixture");
		building.advanceTick();
		building.advanceTick();
		require(!building.lookupAgent(agent).entity->getPath(),
			"Frozen timer used elapsed deactivation ticks");
		building.advanceTick();
		require(building.lookupAgent(agent).entity->getPath()
			&& building.consumeAgentBehaviourRuntimeDiagnostics().empty(),
			"Reactivation did not resume the same instance at the remaining timer duration");
	}

	void interactionOutcomesAreImmutableSemanticValues()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "interactions.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "interactions.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local completed = nil
    return { on_event = function(event, context)
      if event.type == "interaction_completed" then
        if event.name ~= "Working control" or event.result ~= "succeeded"
            or event.reason ~= nil or type(event.interaction) ~= "userdata" then
          error("incorrect completed interaction payload")
        end
        completed = event.interaction
      elseif event.type == "interaction_failed" then
        if event.name ~= "Broken control" or event.reason ~= "failed"
            or event.result ~= nil or type(event.interaction) ~= "userdata"
            or event.interaction == completed then
          error("incorrect failed interaction payload")
        end
        local moved = context.move_to(configuration.destination)
        if not moved.accepted then error(moved.status) end
      else
        error("unexpected interaction event")
      end
      for _, forbidden in ipairs({ "actor", "point", "operations", "request",
          "snapshot", "device_operation", "destination" }) do
        if event[forbidden] ~= nil then error("exposed " .. forbidden) end
      end
      if type(event.tick) ~= "number" or type(event.sequence) ~= "number"
          or pcall(function() event.name = "changed" end)
          or pcall(function() event.interaction.value = 1 end) then
        error("mutable interaction event")
      end
    end }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Interactions",
			"interactions.lua", {
				{ "destination", core::AgentBehaviourSchemaType::Marker }
			});

		core::Building building("Interaction outcomes", 10, 2);
		auto const room = building.addRoom("Room", 0, 0, 0, 10, 1);
		building.addSectorMarker(room, 0, 8.5f, "Destination");
		auto const sector = core::SectorId{ static_cast<uint64_t>(room) + 1 };
		core::InteractionBinding command{
			{ core::DeviceCommandType::SetSectorLights, sector, true },
			core::InteractionBindingRequirement::Required };
		auto const working = building.createInteractionPoint("Working control", sector,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { command });
		auto const broken = building.createInteractionPoint("Broken control", sector,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { command });
		building.finishBuild();
		auto const agent = building.createAgent("Operator", room, 0, 0.5f);
		building.pauseSimulation();
		building.attachAgentBehaviourRegistry("interactions.behaviours", registry);
		require(building.setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			{ { "destination", building.getMarkerIds().front() } }),
			"Could not assign interaction outcome fixture");
		require(building.resumeSimulation(), "Could not start interaction fixture");
		building.advanceTick(); // Construct and start the instance.

		auto const completedRequest = building.requestInteraction(working, agent);
		auto completed = building.lookupInteractionRequest(completedRequest);
		require(completed && !completed.entity->getOperations().empty(),
			"Could not create completed interaction fixture");
		building.lookupDeviceOperation(completed.entity->getOperations().front().first)
			.entity->setState(core::DeviceOperationState::Succeeded);
		building.advanceTick(); // Publish completion.
		building.advanceTick(); // Deliver completion.

		auto const failedRequest = building.requestInteraction(broken, agent);
		auto failed = building.lookupInteractionRequest(failedRequest);
		require(failed && !failed.entity->getOperations().empty(),
			"Could not create failed interaction fixture");
		building.lookupDeviceOperation(failed.entity->getOperations().front().first)
			.entity->setState(core::DeviceOperationState::Failed);
		building.advanceTick(); // Publish failure.
		building.advanceTick(); // Deliver failure and its movement command.
		require(building.lookupAgent(agent).entity->getPath()
			&& building.consumeAgentBehaviourRuntimeDiagnostics().empty(),
			"Immutable semantic interaction outcomes were not delivered correctly");
	}

	void teardownIsReadOnlyAndBestEffort()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "teardown.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "failure.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function()
    return {
      on_start = function(context) context.set_timer("fail", 1) end,
      on_timer = function() error("primary callback failure") end,
      on_stop = function(reason, context)
        if reason ~= "instance_failure" or context.move_to ~= nil
            or context.cancel_movement ~= nil or context.set_timer ~= nil
            or context.cancel_timer ~= nil or context.random_number ~= nil
            or context.random_integer ~= nil or type(context.state.name) ~= "string"
            or pcall(function() context.tick = 0 end) then
          error("on_stop context was not read-only")
        end
        error("best-effort stop failure")
      end
    }
  end
}
)lua");
		auto const failureBehaviour = registry->addAgentBehaviour("Failure teardown",
			"failure.lua", {});
		writeText(package / "unassignment.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function()
    return { on_stop = function(reason, context)
      if reason ~= "unassignment" or context.move_to ~= nil
          or context.set_timer ~= nil or context.random_integer ~= nil then
        error("wrong unassignment teardown")
      end
      error("unassignment stop observed")
    end }
  end
}
)lua");
		auto const unassignmentBehaviour = registry->addAgentBehaviour("Unassignment",
			"unassignment.lua", {});
		writeText(package / "close.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function()
    return { on_stop = function(reason, context)
      if reason ~= "building_close" or context.cancel_movement ~= nil
          or context.cancel_timer ~= nil or context.random_number ~= nil then
        error("wrong Building-close teardown")
      end
      error("close failure must not escape")
    end }
  end
}
)lua");
		auto const closeBehaviour = registry->addAgentBehaviour("Close",
			"close.lua", {});

		auto makeBuilding = [&]
		{
			auto building = std::make_unique<core::Building>("Teardown", 6, 2);
			auto const room = building->addRoom("Room", 0, 0, 0, 6, 1);
			building->finishBuild();
			auto const agent = building->createAgent("Agent", room, 0, 0.5f);
			building->pauseSimulation();
			building->attachAgentBehaviourRegistry("teardown.behaviours", registry);
			return std::pair{ std::move(building), agent };
		};

		{
			auto [building, agent] = makeBuilding();
			require(building->setAgentBehaviourAssignment(agent, failureBehaviour,
				registry->lookupAgentBehaviour(failureBehaviour)->getRevision(), {}),
				"Could not assign failure teardown fixture");
			require(building->resumeSimulation(), "Could not start failure teardown");
			building->advanceTicks(2);
			auto writer = core::YamlSerializer::toString();
			core::SerializationWorkData work;
			work.markSerializedUnmodified = false;
			building->serialize(*writer, work);
			writer->serialize();
			require(writer->getSerializedString().find("primary callback failure")
					== std::string::npos
				&& writer->getSerializedString().find("best-effort stop failure")
					== std::string::npos,
				"Runtime diagnostics entered Building persistence");
			auto diagnostics = building->consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 2
				&& diagnostics[0].callback == "on_timer"
				&& diagnostics[0].diagnostic.find("primary callback failure")
					!= std::string::npos
				&& diagnostics[1].callback == "on_stop"
				&& diagnostics[1].diagnostic.find("best-effort stop failure")
					!= std::string::npos
				&& !building->agentBehaviourOwnsMovement(agent),
				"Instance failure did not complete best-effort teardown");
		}

		{
			auto [building, agent] = makeBuilding();
			require(building->setAgentBehaviourAssignment(agent, unassignmentBehaviour,
				registry->lookupAgentBehaviour(unassignmentBehaviour)->getRevision(), {}),
				"Could not assign unassignment teardown fixture");
			require(building->resumeSimulation(), "Could not start unassignment fixture");
			building->advanceTick();
			building->pauseSimulation();
			require(building->clearAgentBehaviourAssignment(agent),
				"A failing on_stop vetoed unassignment");
			auto diagnostics = building->consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].callback == "on_stop"
				&& diagnostics[0].diagnostic.find("unassignment stop observed")
					!= std::string::npos
				&& !building->agentBehaviourOwnsMovement(agent),
				"Unassignment did not finish after on_stop failed");
		}

		{
			auto [building, agent] = makeBuilding();
			require(building->setAgentBehaviourAssignment(agent, closeBehaviour,
				registry->lookupAgentBehaviour(closeBehaviour)->getRevision(), {}),
				"Could not assign Building-close teardown fixture");
			require(building->resumeSimulation(), "Could not start close fixture");
			building->advanceTick();
			building.reset(); // A failing on_stop must not block or escape close.
		}
	}

	void configuredSchedulesAndRandomStreamsReplay()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "schedule.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "schedule.lua", R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local index = 1
    local reached = 0
    local function move(context)
      local result = context.move_to(configuration.schedule[index].destination)
      if not result.accepted then error(result.status) end
    end
    return {
      on_start = function(context)
        local integer = context.random_integer(-7, 11)
        local number = context.random_number()
        if type(integer) ~= "number" or integer < -7 or integer > 11
            or type(number) ~= "number" or number < 0 or number >= 1 then
          error("deterministic random operation returned an invalid range")
        end
        move(context)
      end,
      on_event = function(event, context)
        if event.type ~= "destination_reached" then return end
        reached = reached + 1
        local jitter = context.random_integer(0, 3)
        local sample = context.random_number()
        if sample < 0 or sample >= 1 then error("invalid random number") end
        context.set_timer("advance", configuration.schedule[index].duration + jitter)
      end,
      on_timer = function(name, context)
        if name ~= "advance" then error("unexpected schedule timer") end
        index = index == 1 and 2 or 1
        move(context)
      end
    }
  end
}
)lua");
		std::vector<core::AgentBehaviourSchemaField> entryFields{
			{ "duration", core::AgentBehaviourSchemaType::Duration },
			{ "destination", core::AgentBehaviourSchemaType::Marker }
		};
		auto const behaviour = registry->addAgentBehaviour("Schedule", "schedule.lua", {
			{ "schedule", core::AgentBehaviourSchemaType::List, {
				{ "entry", core::AgentBehaviourSchemaType::Record, entryFields }
			} }
		});
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The composite schedule fixture did not preflight");

		struct ScheduleBuilding
		{
			std::shared_ptr<core::Building> building;
			core::AgentId first;
			core::AgentId second;
		};
		auto makeBuilding = [&](bool extra, uint64_t seed = 0x1545eedu)
		{
			ScheduleBuilding fixture;
			fixture.building = std::make_shared<core::Building>("Schedules", 36, 2);
			auto const room = fixture.building->addRoom("Room", 0, 0, 0, 36, 1);
			fixture.building->addSectorMarker(room, 0, 4.5f, "Work");
			fixture.building->addSectorMarker(room, 0, 10.5f, "Lunch");
			fixture.building->addSectorMarker(room, 0, 17.5f, "Home");
			fixture.building->addSectorMarker(room, 0, 24.5f, "Gym");
			fixture.building->addSectorMarker(room, 0, 31.5f, "Park");
			fixture.building->finishBuild();
			fixture.first = fixture.building->createAgent("First", room, 0, 0.5f);
			fixture.second = fixture.building->createAgent("Second", room, 0, 1.5f);
			auto const third = extra
				? fixture.building->createAgent("Noisy", room, 0, 2.5f)
				: core::AgentId{};
			fixture.building->pauseSimulation();
			std::string diagnostic;
			require(fixture.building->setRandomSeed(seed, &diagnostic),
				"Could not author the Building random seed: " + diagnostic);
			fixture.building->attachAgentBehaviourRegistry("schedule.behaviours", registry);
			auto const markers = fixture.building->getMarkerIds();
			auto schedule = [](core::MarkerId firstMarker, uint64_t firstDuration,
				core::MarkerId secondMarker, uint64_t secondDuration)
			{
				return core::AgentBehaviourConfiguration{ { "schedule",
					core::AgentBehaviourConfigurationList{
						core::AgentBehaviourConfigurationRecord{
							{ "duration", core::AgentBehaviourDuration{ firstDuration } },
							{ "destination", firstMarker } },
						core::AgentBehaviourConfigurationRecord{
							{ "duration", core::AgentBehaviourDuration{ secondDuration } },
							{ "destination", secondMarker } }
					} } };
			};
			auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
			require(fixture.building->setAgentBehaviourAssignment(fixture.first,
				behaviour, revision, schedule(markers[0], 2, markers[1], 3), &diagnostic)
				&& fixture.building->setAgentBehaviourAssignment(fixture.second,
					behaviour, revision, schedule(markers[2], 4, markers[3], 1), &diagnostic),
				"Could not assign distinct composite schedules: " + diagnostic);
			if (third)
				require(fixture.building->setAgentBehaviourAssignment(third,
					behaviour, revision, schedule(markers[4], 1, markers[4], 1), &diagnostic),
					"Could not assign the independent-stream noise Agent");
			return fixture;
		};

		auto run = [](ScheduleBuilding const& fixture)
		{
			if (fixture.building->isSimulationPaused())
				require(fixture.building->resumeSimulation(),
					"Could not resume a schedule replay");
			fixture.building->consumeSimulationEvents();
			std::ostringstream digest;
			unsigned firstReached = 0, secondReached = 0;
			for (unsigned tick = 0; tick < 5000
				&& (firstReached < 4 || secondReached < 4); ++tick)
			{
				if (tick == 10)
				{
					fixture.building->pauseSimulation();
					require(fixture.building->resumeSimulation(),
						"Pause/resume did not preserve schedule state");
				}
				fixture.building->advanceTick();
				for (auto const& event : fixture.building->consumeSimulationEvents())
				{
					if (event.type != core::SimulationEventType::DestinationReached
						|| (event.agent.id != fixture.first
							&& event.agent.id != fixture.second)) continue;
					if (event.agent.id == fixture.first)
					{
						if (firstReached >= 4) continue;
						++firstReached;
					}
					else
					{
						if (secondReached >= 4) continue;
						++secondReached;
					}
					digest << event.tick << ':' << event.agent.id.value << ':'
						<< event.destinationMarker.value << '|';
				}
			}
			auto diagnostics = fixture.building->consumeAgentBehaviourRuntimeDiagnostics();
			std::string detail = " (first=" + std::to_string(firstReached)
				+ ", second=" + std::to_string(secondReached) + ")";
			if (!diagnostics.empty()) detail += ": " + diagnostics.front().diagnostic;
			require(firstReached == 4 && secondReached == 4,
				"The shared schedule module did not advance both private schedules" + detail);
			require(diagnostics.empty(),
				"A configured schedule or random operation failed" + detail);
			return digest.str();
		};

		auto fixture = makeBuilding(false);
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData writeWork;
		writeWork.markSerializedUnmodified = false;
		fixture.building->serialize(*writer, writeWork);
		writer->serialize();
		auto const authoredYaml = writer->getSerializedString();
		require(authoredYaml.find("randomSeed: 22306541") != std::string::npos,
			"The authored Building random seed was not persisted");

		auto const first = run(fixture);
		fixture.building->resetSimulation();
		require(fixture.building->getRandomSeed() == 0x1545eedu,
			"Simulation reset lost the authored Building random seed");
		auto const afterReset = run(fixture);
		require(first == afterReset,
			"Simulation reset did not recreate schedule state and random streams");

		auto reopened = std::make_shared<core::Building>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(authoredYaml);
		reader->deserialize();
		core::SerializationWorkData readWork;
		require(reopened->deserialize(*reader, readWork),
			"The authored schedule Building did not reload");
		reopened->resolveAgentBehaviourRegistry(registry);
		ScheduleBuilding loaded{ reopened, fixture.first, fixture.second };
		require(run(loaded) == first,
			"Save/load did not reproduce configured schedule outcomes");

		auto noisy = makeBuilding(true);
		require(run(noisy) == first,
			"Another Agent's callbacks altered an independent random stream");
		auto differentSeed = makeBuilding(false, 0x1545eedu + 1u);
		require(run(differentSeed) != first,
			"The authored Building seed did not affect deterministic random streams");
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
	prohibitedHostSurfacesAreAbsent();
	customLoaderIsReservedAndImmutable();
	scratchExecutionIsBudgeted();
	liveLoadsFactoriesAndCallbacksAreContained();
	independentStartupInstancesMoveDeterministically();
	manifestHelpersHavePrivatePerAgentGraphs();
	routeLossAndTopologyLifecycle();
	programmingErrorDisablesMovementOwnership();
	deterministicTimersExposeOnlySemanticState();
	activationSuspendsStateAndFreezesTimers();
	interactionOutcomesAreImmutableSemanticValues();
	teardownIsReadOnlyAndBestEffort();
	configuredSchedulesAndRandomStreamsReplay();
	registryRetainsLoadedAndErrorStatus();
}
