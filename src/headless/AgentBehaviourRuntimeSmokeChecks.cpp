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
#include "core/World.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/Log.h"
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
			core::World world("Instruction containment", 6, 2,
				{ 64u * 1024u * 1024u, 1'000u });
			auto const room = world.addRoom("Room", 0, 0, 0, 6, 1);
			world.finishBuild();
			auto const agent = world.createAgent("Abusive", room, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("abuse.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, behaviour,
				registry->lookupAgentBehaviour(behaviour)->getRevision(), {}),
				"Could not assign an instruction abuse fixture");
			require(world.getAgentBehaviourRuntimeLimits().instructionsPerCall == 1'000u,
				"The per-World instruction limit was not retained");
			require(world.resumeSimulation(),
				"Could not resume an instruction abuse fixture");
			world.advanceTick();
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1
				&& diagnostics[0].failure
					== core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded
				&& diagnostics[0].stage == expectedStage
				&& diagnostics[0].agent == agent
				&& !world.agentBehaviourOwnsMovement(agent),
				"Instruction exhaustion escaped, lacked structure, or retained ownership");
		};
		runInstructionStage(loadBudget,
			core::AgentBehaviourRuntimeStage::ModuleLoad);
		runInstructionStage(factoryBudget,
			core::AgentBehaviourRuntimeStage::Factory);
		runInstructionStage(callbackBudget,
			core::AgentBehaviourRuntimeStage::Callback);

		{
			core::World world("Memory recovery", 8, 2,
				{ 2u * 1024u * 1024u, 100'000u });
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const abusive = world.createAgent("Abusive", room, 0, 0.5f);
			auto const healthy = world.createAgent("Healthy", room, 0, 1.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("abuse.behaviours", registry);
			require(world.getAgentBehaviourRuntimeLimits().memoryBytes
					== 2u * 1024u * 1024u,
				"The per-World heap limit was not retained");
			require(world.setAgentBehaviourAssignment(abusive, memoryBudget,
				registry->lookupAgentBehaviour(memoryBudget)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(healthy, safe,
					registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not assign the live allocator recovery fixtures");
			// The healthy callback exercises a host capability after the refused
			// allocation while the failed instance releases its Lua heap graph.
			require(world.resumeSimulation(),
				"Could not resume the live allocator fixture");
			world.advanceTick();
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1
				&& diagnostics[0].failure
					== core::AgentBehaviourRuntimeFailure::MemoryBudgetExceeded
				&& diagnostics[0].stage == core::AgentBehaviourRuntimeStage::Callback
				&& diagnostics[0].callback == "on_start"
				&& diagnostics[0].agent == abusive
				&& !world.agentBehaviourOwnsMovement(abusive)
				&& world.agentBehaviourOwnsMovement(healthy),
				"Live heap exhaustion corrupted the state or disabled a healthy instance");

			world.pauseSimulation();
			require(world.clearAgentBehaviourAssignment(abusive),
				"Could not remove the exhausted instance during recovery");
			require(world.setAgentBehaviourAssignment(abusive, safe,
				registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not create a replacement after refused allocation");
			require(world.resumeSimulation(),
				"Could not resume after refused allocation");
			world.advanceTick();
			require(world.consumeAgentBehaviourRuntimeDiagnostics().empty()
				&& world.agentBehaviourOwnsMovement(abusive),
				"The World Lua state did not recover after a refused allocation");
		}

		{
			core::World world("Lua error containment", 6, 2);
			auto const room = world.addRoom("Room", 0, 0, 0, 6, 1);
			world.finishBuild();
			auto const agent = world.createAgent("Abusive", room, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("abuse.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, luaError,
				registry->lookupAgentBehaviour(luaError)->getRevision(), {}),
				"Could not assign the protected Lua error fixture");
			require(world.resumeSimulation(),
				"Could not resume the protected Lua error fixture");
			world.advanceTick();
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
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
		core::World world("Lua startup", 14, 2);
		auto const room = world.addRoom("Room", 0, 0, 0, 14, 1);
		world.addSectorMarker(room, 0, 4.5f, "Near");
		world.addSectorMarker(room, 0, 11.5f, "Far");
		world.finishBuild();
		auto const first = world.createAgent("First", room, 0, 0.5f);
		auto const second = world.createAgent("Second", room, 0, 1.5f);
		auto const markers = world.getMarkerIds();

		world.pauseSimulation();
		world.consumeSimulationEvents();
		world.attachAgentBehaviourRegistry("startup.behaviours", registry);
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		require(world.setAgentBehaviourAssignment(first, behaviour, revision,
			{ { "destination", markers[0] } }),
			"Could not assign the first startup behaviour");
		require(world.setAgentBehaviourAssignment(second, behaviour, revision,
			{ { "destination", markers[1] } }),
			"Could not assign the second startup behaviour");
		require(world.agentBehaviourOwnsMovement(first)
			&& world.agentBehaviourOwnsMovement(second),
			"Assigned enabled behaviours did not acquire movement ownership");
		require(world.moveAgentToMarker(first, markers[0]).status
				== core::MovementCommandStatus::BehaviourOwned
			&& world.cancelAgentMovement(first).status
				== core::MovementCommandStatus::BehaviourOwned,
			"Manual movement commands bypassed behaviour ownership");
		auto firstAgent = world.lookupAgent(first).entity;
		auto manualTarget = world.getGraph()->getClosestVertexInSector(
			firstAgent->getSector(), { 3.5f, 0.0f });
		firstAgent->setPath(world.getGraph()->calculatePath(firstAgent, manualTarget), true);
		require(!firstAgent->getPath(),
			"Direct manual Path assignment bypassed behaviour ownership");
		require(world.resumeSimulation(), "Could not resume the Lua startup fixture");
		world.consumeSimulationEvents();

		world.advanceTick();
		auto firstTick = world.getSimulationSnapshot();
		require(firstTick.tick == 1 && firstTick.agents.size() == 2
			&& firstTick.agents[0].hasPath && firstTick.agents[1].hasPath
			&& firstTick.agents[0].globalPosition.x > 0.5f
			&& firstTick.agents[1].globalPosition.x > 1.5f,
			"on_start movement was not applied before first-tick intent and movement");

		std::ostringstream digest;
		unsigned reached = 0;
		auto observePublicEvents = [&]
		{
			for (auto const& event : world.consumeSimulationEvents())
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
			world.advanceTick();
			observePublicEvents();
		}
		require(reached == 2,
			"Independently configured Lua Agents did not reach both Markers");

		auto const completed = world.getSimulationSnapshot();
		require(std::fabs(completed.agents[0].globalPosition.x - 4.5f) < 0.001f
			&& std::fabs(completed.agents[1].globalPosition.x - 11.5f) < 0.001f,
			"Shared behaviour instances did not retain distinct Marker configuration");
		world.advanceTicks(5);
		observePublicEvents();
		require(reached == 2, "on_start or destination_reached delivery ran more than once");
		require(world.agentBehaviourOwnsMovement(first)
			&& world.agentBehaviourOwnsMovement(second),
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
			"Per-World Lua startup and movement were not deterministic");
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
			"Private helper graphs were not deterministic across World runtimes");

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
			core::World world(disconnect ? "Lost route" : "Replacement route", 8, 2);
			auto const front = world.addRoom("Front", 0, 0, 0, 8, 1);
			auto const back = world.addRoom("Back", 1, 0, 0, 8, 1);
			auto const door = world.addSectorDoor(front, 0, 2, {});
			world.addSectorMarker(back, 0, 6.5f, "Destination");
			world.addSectorMarker(front, 0, 0.5f, "Fallback");
			world.finishBuild();
			auto const markers = world.getMarkerIds();
			auto const id = world.createAgent("Walker", front, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("lifecycle.behaviours", registry);
			auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
			require(world.setAgentBehaviourAssignment(id, behaviour, revision, {
				{ "destination", markers[0] }, { "fallback", markers[1] },
				{ "expected_reason", std::string("topology_changed") }
			}), "Could not assign topology lifecycle behaviour");
			require(world.resumeSimulation(), "Could not start topology lifecycle fixture");
			world.consumeSimulationEvents();
			world.advanceTicks(5);
			world.consumeSimulationEvents();
			world.pauseSimulation();
			if (disconnect)
				require(world.removeSectorDoor(front, door.door.index),
					"Could not remove the topology fixture Door");
			world.finishBuild();
			require(world.resumeSimulation(), "Could not resume rebuilt topology fixture");
			world.consumeSimulationEvents();

			unsigned losses = 0, reached = 0;
			core::MarkerId reachedMarker;
			for (unsigned tick = 0; tick < 3000 && reached == 0; ++tick)
			{
				world.advanceTick();
				for (auto const& event : world.consumeSimulationEvents())
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

		core::World unreachable("Initial route loss", 12, 2);
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
		core::World world("Programming error", 8, 2);
		auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
		world.addSectorMarker(room, 0, 6.5f, "Destination");
		world.addSectorMarker(room, 0, 0.5f, "Return");
		world.finishBuild();
		auto const markers = world.getMarkerIds();
		auto const marker = markers[0];
		auto const id = world.createAgent("Walker", room, 0, 0.5f);
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("programming-error.behaviours", registry);
		require(world.setAgentBehaviourAssignment(id, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			{ { "destination", marker } }), "Could not assign duplicate-command fixture");
		require(world.resumeSimulation(), "Could not start duplicate-command fixture");
		world.consumeSimulationEvents();
		world.advanceTick();
		require(!world.agentBehaviourOwnsMovement(id)
			&& !world.lookupAgent(id).entity->getPath(),
			"A multiple-movement-command callback partially applied or retained ownership");
		require(world.isSimulationPaused(),
			"A callback failure did not pause before the next tick");
		require(world.moveAgentToMarker(id, marker).accepted(),
			"Disabling the failed instance did not restore manual movement controls");
		require(world.resumeSimulation(),
			"Could not resume after acknowledging the callback failure");
		world.advanceTicks(1000);
		unsigned reached = 0;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::DestinationReached) ++reached;
		require(reached == 1, "Manual movement did not work after instance disablement");

		world.pauseSimulation();
		require(world.clearAgentBehaviourAssignment(id),
			"Could not unassign the disabled behaviour");
		require(!world.agentBehaviourOwnsMovement(id),
			"Unassignment left runtime movement ownership behind");

		require(world.setAgentBehaviourAssignment(id, movingBehaviour,
			registry->lookupAgentBehaviour(movingBehaviour)->getRevision(),
			{ { "destination", markers[1] } }),
			"Could not assign the active-unassignment fixture");
		require(world.resumeSimulation(), "Could not start active-unassignment fixture");
		world.advanceTick();
		require(world.lookupAgent(id).entity->getPath()
			&& world.agentBehaviourOwnsMovement(id),
			"The active-unassignment fixture did not acquire a route");
		world.pauseSimulation();
		require(world.clearAgentBehaviourAssignment(id),
			"Could not unassign an actively moving behaviour");
		require(!world.agentBehaviourOwnsMovement(id)
			&& !world.lookupAgent(id).entity->getPath(),
			"Active unassignment retained runtime movement ownership");
		require(world.resumeSimulation(), "Could not resume after active unassignment");
		require(world.moveAgentToMarker(id, markers[1]).accepted(),
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

		core::World world("Timers", 16, 2,
			{ 64u * 1024u * 1024u, 100'000u, 2u });
		auto const room = world.addRoom("Room", 0, 0, 0, 16, 1);
		world.addSectorMarker(room, 0, 13.5f, "Destination");
		world.finishBuild();
		auto const overflow = world.createAgent("Overflow", room, 0, 0.5f);
		auto const first = world.createAgent("First", room, 0, 1.5f);
		auto const second = world.createAgent("Second", room, 0, 2.5f);
		auto const orderFirst = world.createAgent("Order first", room, 0, 3.5f);
		auto const orderSecond = world.createAgent("Order second", room, 0, 4.5f);
		auto const destination = world.getMarkerIds().front();
		world.pauseSimulation();
		world.consumeSimulationEvents();
		world.attachAgentBehaviourRegistry("timers.behaviours", registry);
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		auto assign = [&](core::AgentId id, std::string name, bool exceedsLimit)
		{
			require(world.setAgentBehaviourAssignment(id, behaviour, revision, {
				{ "expected_name", std::move(name) }, { "destination", destination },
				{ "overflow", exceedsLimit }
			}), "Could not assign deterministic timer fixture");
		};
		assign(overflow, "Overflow", true);
		assign(first, "First", false);
		assign(second, "Second", false);
		auto const orderingRevision = registry->lookupAgentBehaviour(
			orderingBehaviour)->getRevision();
		require(world.setAgentBehaviourAssignment(orderFirst, orderingBehaviour,
				orderingRevision, {})
			&& world.setAgentBehaviourAssignment(orderSecond, orderingBehaviour,
				orderingRevision, {}),
			"Could not assign callback-order timer fixtures");
		require(world.getAgentBehaviourRuntimeLimits().timersPerInstance == 2,
			"The configured per-instance timer limit was not retained");
		require(world.resumeSimulation(), "Could not resume timer fixture");
		world.consumeSimulationEvents();

		std::ostringstream digest;
		unsigned phaseEvents = 0;
		auto consume = [&]
		{
			for (auto const& event : world.consumeSimulationEvents())
			{
				if (event.type == core::SimulationEventType::PhaseCompleted) ++phaseEvents;
				if (event.type == core::SimulationEventType::AgentChanged
					|| event.type == core::SimulationEventType::DestinationReached)
					digest << event.tick << ':' << event.sequence << ':'
						<< static_cast<unsigned>(event.type) << ':'
						<< event.agent.id.value << '|';
			}
		};
		// The overflowing startup batch is reported headlessly and pauses before
		// tick 1. Healthy instances retain their complete startup batches.
		require(!world.advanceTick() && world.isSimulationPaused(),
			"A timer storm did not stop the headless boundary visibly");
		require(world.resumeSimulation(),
			"Could not resume after acknowledging the timer storm");
		consume();
		// Tick 1 first completes; its due timers run at the following boundary.
		require(world.advanceTick(),
			"Healthy startup batches did not permit tick 1 to complete");
		consume();
		// Both ordered timer failures occur at that boundary in Agent-ID order,
		// after healthy timer callbacks have completed atomically.
		require(!world.advanceTick() && world.isSimulationPaused(),
			"Ordered callback failures did not stop the headless boundary");
		require(world.resumeSimulation(),
			"Could not resume after acknowledging ordered callback failures");
		consume();
		for (unsigned tick = 0; tick < 4; ++tick)
		{
			require(world.advanceTick(),
				"A healthy timer callback unexpectedly stopped the run");
			consume();
		}
		auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
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
		require(!world.agentBehaviourOwnsMovement(overflow)
			&& world.agentBehaviourOwnsMovement(first)
			&& world.agentBehaviourOwnsMovement(second),
			"One instance's timer limit affected another instance");
		require(world.lookupAgent(first).entity->getPath()
			&& world.lookupAgent(second).entity->getPath(),
			"Lexically ordered one-shot timers did not apply their movement commands");

		unsigned reached = 0;
		for (unsigned tick = 0; tick < 2000 && reached < 2; ++tick)
		{
			world.advanceTick();
			for (auto const& event : world.consumeSimulationEvents())
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
		world.advanceTicks(5);
		consume();
		require(world.consumeAgentBehaviourRuntimeDiagnostics().empty(),
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

		core::World world("Activation lifetime", 12, 2);
		auto const room = world.addRoom("Room", 0, 0, 0, 12, 1);
		world.addSectorMarker(room, 0, 10.5f, "Destination");
		world.finishBuild();
		auto const agent = world.createAgent("Sleeper", room, 0, 0.5f);
		auto const destination = world.getMarkerIds().front();
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("activation.behaviours", registry);
		require(world.setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			{ { "destination", destination } }),
			"Could not assign activation lifetime fixture");
		require(world.resumeSimulation(), "Could not start activation fixture");
		world.consumeSimulationEvents();
		world.advanceTick(); // on_start at tick 0; timer due at tick 3.
		world.pauseSimulation();
		world.consumeSimulationEvents();
		require(world.setAgentActive(agent, false), "Could not deactivate Agent");
		require(world.setAgentActive(agent, false),
			"Idempotent deactivation was refused");
		unsigned deactivatedEvents = 0;
		for (auto const& event : world.consumeSimulationEvents())
			deactivatedEvents += event.type
				== core::SimulationEventType::AgentDeactivated;
		require(deactivatedEvents == 1,
			"Deactivation did not publish exactly one semantic transition");

		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("secret_instance_155") == std::string::npos
			&& yaml.find("frozen_timer_155") == std::string::npos,
			"Private instance state or timers entered World persistence");

		require(world.resumeSimulation(), "Could not run deactivated fixture");
		world.advanceTicks(5);
		require(!world.lookupAgent(agent).entity->getPath(),
			"A suspended timer or command moved a deactivated Agent");
		world.pauseSimulation();
		world.consumeSimulationEvents();
		require(world.setAgentActive(agent, true), "Could not reactivate Agent");
		unsigned activatedEvents = 0;
		for (auto const& event : world.consumeSimulationEvents())
			activatedEvents += event.type == core::SimulationEventType::AgentActivated;
		require(activatedEvents == 1,
			"Reactivation did not publish exactly one semantic transition");
		require(world.resumeSimulation(), "Could not resume reactivated fixture");
		world.advanceTick();
		world.advanceTick();
		require(!world.lookupAgent(agent).entity->getPath(),
			"Frozen timer used elapsed deactivation ticks");
		world.advanceTick();
		require(world.lookupAgent(agent).entity->getPath()
			&& world.consumeAgentBehaviourRuntimeDiagnostics().empty(),
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

		core::World world("Interaction outcomes", 10, 2);
		auto const room = world.addRoom("Room", 0, 0, 0, 10, 1);
		world.addSectorMarker(room, 0, 8.5f, "Destination");
		auto const sector = core::SectorId{ static_cast<uint64_t>(room) + 1 };
		core::InteractionBinding command{
			{ core::DeviceCommandType::SetSectorLights, sector, true },
			core::InteractionBindingRequirement::Required };
		auto const working = world.createInteractionPoint("Working control", sector,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { command });
		auto const broken = world.createInteractionPoint("Broken control", sector,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { command });
		world.finishBuild();
		auto const agent = world.createAgent("Operator", room, 0, 0.5f);
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("interactions.behaviours", registry);
		require(world.setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			{ { "destination", world.getMarkerIds().front() } }),
			"Could not assign interaction outcome fixture");
		require(world.resumeSimulation(), "Could not start interaction fixture");
		world.advanceTick(); // Construct and start the instance.

		auto const completedRequest = world.requestInteraction(working, agent);
		auto completed = world.lookupInteractionRequest(completedRequest);
		require(completed && !completed.entity->getOperations().empty(),
			"Could not create completed interaction fixture");
		world.lookupDeviceOperation(completed.entity->getOperations().front().first)
			.entity->setState(core::DeviceOperationState::Succeeded);
		world.advanceTick(); // Publish completion.
		world.advanceTick(); // Deliver completion.

		auto const failedRequest = world.requestInteraction(broken, agent);
		auto failed = world.lookupInteractionRequest(failedRequest);
		require(failed && !failed.entity->getOperations().empty(),
			"Could not create failed interaction fixture");
		world.lookupDeviceOperation(failed.entity->getOperations().front().first)
			.entity->setState(core::DeviceOperationState::Failed);
		world.advanceTick(); // Publish failure.
		world.advanceTick(); // Deliver failure and its movement command.
		require(world.lookupAgent(agent).entity->getPath()
			&& world.consumeAgentBehaviourRuntimeDiagnostics().empty(),
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
      if reason ~= "world_close" or context.cancel_movement ~= nil
          or context.cancel_timer ~= nil or context.random_number ~= nil then
        error("wrong World-close teardown")
      end
      error("close failure must not escape")
    end }
  end
}
)lua");
		auto const closeBehaviour = registry->addAgentBehaviour("Close",
			"close.lua", {});

		auto makeWorld = [&]
		{
			auto world = std::make_unique<core::World>("Teardown", 6, 2);
			auto const room = world->addRoom("Room", 0, 0, 0, 6, 1);
			world->finishBuild();
			auto const agent = world->createAgent("Agent", room, 0, 0.5f);
			world->pauseSimulation();
			world->attachAgentBehaviourRegistry("teardown.behaviours", registry);
			return std::pair{ std::move(world), agent };
		};

		{
			auto [world, agent] = makeWorld();
			require(world->setAgentBehaviourAssignment(agent, failureBehaviour,
				registry->lookupAgentBehaviour(failureBehaviour)->getRevision(), {}),
				"Could not assign failure teardown fixture");
			require(world->resumeSimulation(), "Could not start failure teardown");
			world->advanceTicks(2);
			auto writer = core::YamlSerializer::toString();
			core::SerializationWorkData work;
			work.markSerializedUnmodified = false;
			world->serialize(*writer, work);
			writer->serialize();
			require(writer->getSerializedString().find("primary callback failure")
					== std::string::npos
				&& writer->getSerializedString().find("best-effort stop failure")
					== std::string::npos,
				"Runtime diagnostics entered World persistence");
			auto diagnostics = world->consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 2
				&& diagnostics[0].callback == "on_timer"
				&& diagnostics[0].diagnostic.find("primary callback failure")
					!= std::string::npos
				&& diagnostics[1].callback == "on_stop"
				&& diagnostics[1].diagnostic.find("best-effort stop failure")
					!= std::string::npos
				&& !world->agentBehaviourOwnsMovement(agent),
				"Instance failure did not complete best-effort teardown");
		}

		{
			auto [world, agent] = makeWorld();
			require(world->setAgentBehaviourAssignment(agent, unassignmentBehaviour,
				registry->lookupAgentBehaviour(unassignmentBehaviour)->getRevision(), {}),
				"Could not assign unassignment teardown fixture");
			require(world->resumeSimulation(), "Could not start unassignment fixture");
			world->advanceTick();
			world->pauseSimulation();
			require(world->clearAgentBehaviourAssignment(agent),
				"A failing on_stop vetoed unassignment");
			auto diagnostics = world->consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].callback == "on_stop"
				&& diagnostics[0].diagnostic.find("unassignment stop observed")
					!= std::string::npos
				&& !world->agentBehaviourOwnsMovement(agent),
				"Unassignment did not finish after on_stop failed");
		}

		{
			auto [world, agent] = makeWorld();
			require(world->setAgentBehaviourAssignment(agent, closeBehaviour,
				registry->lookupAgentBehaviour(closeBehaviour)->getRevision(), {}),
				"Could not assign World-close teardown fixture");
			require(world->resumeSimulation(), "Could not start close fixture");
			world->advanceTick();
			world.reset(); // A failing on_stop must not block or escape close.
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

		struct ScheduleWorld
		{
			std::shared_ptr<core::World> world;
			core::AgentId first;
			core::AgentId second;
		};
		auto makeWorld = [&](bool extra, uint64_t seed = 0x1545eedu)
		{
			ScheduleWorld fixture;
			fixture.world = std::make_shared<core::World>("Schedules", 36, 2);
			auto const room = fixture.world->addRoom("Room", 0, 0, 0, 36, 1);
			fixture.world->addSectorMarker(room, 0, 4.5f, "Work");
			fixture.world->addSectorMarker(room, 0, 10.5f, "Lunch");
			fixture.world->addSectorMarker(room, 0, 17.5f, "Home");
			fixture.world->addSectorMarker(room, 0, 24.5f, "Gym");
			fixture.world->addSectorMarker(room, 0, 31.5f, "Park");
			fixture.world->finishBuild();
			fixture.first = fixture.world->createAgent("First", room, 0, 0.5f);
			fixture.second = fixture.world->createAgent("Second", room, 0, 1.5f);
			auto const third = extra
				? fixture.world->createAgent("Noisy", room, 0, 2.5f)
				: core::AgentId{};
			fixture.world->pauseSimulation();
			std::string diagnostic;
			require(fixture.world->setRandomSeed(seed, &diagnostic),
				"Could not author the World random seed: " + diagnostic);
			fixture.world->attachAgentBehaviourRegistry("schedule.behaviours", registry);
			auto const markers = fixture.world->getMarkerIds();
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
			require(fixture.world->setAgentBehaviourAssignment(fixture.first,
				behaviour, revision, schedule(markers[0], 2, markers[1], 3), &diagnostic)
				&& fixture.world->setAgentBehaviourAssignment(fixture.second,
					behaviour, revision, schedule(markers[2], 4, markers[3], 1), &diagnostic),
				"Could not assign distinct composite schedules: " + diagnostic);
			if (third)
				require(fixture.world->setAgentBehaviourAssignment(third,
					behaviour, revision, schedule(markers[4], 1, markers[4], 1), &diagnostic),
					"Could not assign the independent-stream noise Agent");
			return fixture;
		};

		auto run = [](ScheduleWorld const& fixture)
		{
			if (fixture.world->isSimulationPaused())
				require(fixture.world->resumeSimulation(),
					"Could not resume a schedule replay");
			fixture.world->consumeSimulationEvents();
			std::ostringstream digest;
			unsigned firstReached = 0, secondReached = 0;
			for (unsigned tick = 0; tick < 5000
				&& (firstReached < 4 || secondReached < 4); ++tick)
			{
				if (tick == 10)
				{
					fixture.world->pauseSimulation();
					require(fixture.world->resumeSimulation(),
						"Pause/resume did not preserve schedule state");
				}
				fixture.world->advanceTick();
				for (auto const& event : fixture.world->consumeSimulationEvents())
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
			auto diagnostics = fixture.world->consumeAgentBehaviourRuntimeDiagnostics();
			std::string detail = " (first=" + std::to_string(firstReached)
				+ ", second=" + std::to_string(secondReached) + ")";
			if (!diagnostics.empty()) detail += ": " + diagnostics.front().diagnostic;
			require(firstReached == 4 && secondReached == 4,
				"The shared schedule module did not advance both private schedules" + detail);
			require(diagnostics.empty(),
				"A configured schedule or random operation failed" + detail);
			return digest.str();
		};

		auto fixture = makeWorld(false);
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData writeWork;
		writeWork.markSerializedUnmodified = false;
		fixture.world->serialize(*writer, writeWork);
		writer->serialize();
		auto const authoredYaml = writer->getSerializedString();
		require(authoredYaml.find("randomSeed: 22306541") != std::string::npos,
			"The authored World random seed was not persisted");

		auto const first = run(fixture);
		fixture.world->resetSimulation();
		require(fixture.world->getRandomSeed() == 0x1545eedu,
			"Simulation reset lost the authored World random seed");
		auto const afterReset = run(fixture);
		require(first == afterReset,
			"Simulation reset did not recreate schedule state and random streams");

		auto reopened = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(authoredYaml);
		reader->deserialize();
		core::SerializationWorkData readWork;
		require(reopened->deserialize(*reader, readWork),
			"The authored schedule World did not reload");
		reopened->resolveAgentBehaviourRegistry(registry);
		ScheduleWorld loaded{ reopened, fixture.first, fixture.second };
		require(run(loaded) == first,
			"Save/load did not reproduce configured schedule outcomes");

		auto noisy = makeWorld(true);
		require(run(noisy) == first,
			"Another Agent's callbacks altered an independent random stream");
		auto differentSeed = makeWorld(false, 0x1545eedu + 1u);
		require(run(differentSeed) != first,
			"The authored World seed did not affect deterministic random streams");
	}

	std::string runBoundedStormAndFailureFixture()
	{
		(void)core::consumeLogMessages();
		TemporaryDirectory temporary;
		auto const package = temporary.path / "storms.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		auto add = [&](std::string const& name, std::string const& file,
			std::string const& source)
		{
			writeText(package / file, source);
			return registry->addAgentBehaviour(name, file, {});
		};
		auto const callbackStorm = add("Callback storm", "callbacks.lua", R"lua(
return { api_version = 1, factory = function()
  return {
    on_start = function(context)
      for i = 1, 4 do context.set_timer(string.format("%02d", i), 1) end
    end,
    on_timer = function() end
  }
end }
)lua");
		auto const commandStorm = add("Command storm", "commands.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context)
    for i = 1, 33 do context.set_timer("timer-" .. i, 10) end
  end }
end }
)lua");
		auto const safe = add("Safe", "safe-storm.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function(context) context.set_timer("safe", 20) end }
end }
)lua");
		auto const loadFailure = add("Load failure", "load-storm.lua", R"lua(
local total = 0
for i = 1, 5000 do total = total + i end
return { api_version = 1, factory = function() return {} end }
)lua");
		auto const logging = add("Logging", "logging.lua", R"lua(
return { api_version = 1, factory = function()
  return {
    on_start = function(context)
      for i = 1, 105 do context.log("message-" .. i, "info") end
      context.set_timer("next-window", 600)
    end,
    on_timer = function(_, context) context.log("new-window", "warning") end
  }
end }
)lua");

		std::ostringstream digest;
		{
			core::World world("Callback storm", 8, 2,
				{ 64u * 1024u * 1024u, 100'000u, 256u, 3u, 32u, 100u, 600u });
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const storm = world.createAgent("Storm", room, 0, 0.5f);
			auto const unaffected = world.createAgent("Unaffected", room, 0, 1.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(storm, callbackStorm,
				registry->lookupAgentBehaviour(callbackStorm)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(unaffected, safe,
					registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not assign callback-storm fixtures");
			require(world.resumeSimulation() && world.advanceTick(),
				"Callback-storm startup did not complete");
			require(!world.advanceTick() && world.isSimulationPaused()
				&& world.getSimulationTick() == 1,
				"Callback storm did not stop before the overflowing tick");
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].agent == storm
				&& diagnostics[0].behaviour == callbackStorm
				&& diagnostics[0].agentName == "Storm"
				&& diagnostics[0].behaviourName == "Callback storm"
				&& diagnostics[0].callback == "on_timer" && diagnostics[0].tick == 1
				&& diagnostics[0].diagnostic.find("limit of 3") != std::string::npos
				&& !diagnostics[0].traceback.empty()
				&& !world.agentBehaviourOwnsMovement(storm)
				&& world.agentBehaviourOwnsMovement(unaffected),
				"Callback-storm diagnostic, isolation, or ordering changed");
			digest << diagnostics[0].agent.value << ':' << diagnostics[0].tick << ':'
				<< diagnostics[0].diagnostic << '|';
		}

		{
			core::World world("Command storm", 8, 2);
			auto const defaults = world.getAgentBehaviourRuntimeLimits();
			require(defaults.callbacksPerBoundary == 10'000u
				&& defaults.commandsPerCallback == 32u
				&& defaults.timersPerInstance == 256u
				&& defaults.logMessagesPerWindow == 100u
				&& defaults.logWindowTicks == 600u,
				"Agent behaviour storm defaults changed");
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const storm = world.createAgent("Commands", room, 0, 0.5f);
			auto const unaffected = world.createAgent("Safe", room, 0, 1.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(storm, commandStorm,
				registry->lookupAgentBehaviour(commandStorm)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(unaffected, safe,
					registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not assign command-storm fixtures");
			require(world.resumeSimulation() && !world.advanceTick()
				&& world.isSimulationPaused() && world.getSimulationTick() == 0,
				"Command storm partially entered the overflowing tick");
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			auto const commandDetail = diagnostics.empty() ? std::string("no diagnostic")
				: diagnostics[0].diagnostic;
			require(diagnostics.size() == 1 && diagnostics[0].agent == storm
				&& diagnostics[0].callback == "on_start"
				&& diagnostics[0].diagnostic.find("limit of 32") != std::string::npos
				&& !world.agentBehaviourOwnsMovement(storm)
				&& world.agentBehaviourOwnsMovement(unaffected),
				"Command storm partially applied or affected an unrelated Agent: "
					+ commandDetail + " (count=" + std::to_string(diagnostics.size())
					+ ", stormOwns=" + std::to_string(world.agentBehaviourOwnsMovement(storm))
					+ ", safeOwns=" + std::to_string(world.agentBehaviourOwnsMovement(unaffected)) + ")");
			digest << diagnostics[0].agent.value << ':' << diagnostics[0].tick << ':'
				<< diagnostics[0].diagnostic << '|';
		}

		{
			core::World world("Module failure", 8, 2,
				{ 64u * 1024u * 1024u, 1'000u });
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const first = world.createAgent("First affected", room, 0, 0.5f);
			auto const second = world.createAgent("Second affected", room, 0, 1.5f);
			auto const unaffected = world.createAgent("Other module", room, 0, 2.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(first, loadFailure,
				registry->lookupAgentBehaviour(loadFailure)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(second, loadFailure,
					registry->lookupAgentBehaviour(loadFailure)->getRevision(), {})
				&& world.setAgentBehaviourAssignment(unaffected, safe,
					registry->lookupAgentBehaviour(safe)->getRevision(), {}),
				"Could not assign module-scope fixtures");
			require(world.resumeSimulation() && !world.advanceTick()
				&& world.isSimulationPaused(),
				"Module failure did not stop the headless run");
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].agent == first
				&& diagnostics[0].behaviour == loadFailure
				&& diagnostics[0].stage == core::AgentBehaviourRuntimeStage::ModuleLoad
				&& !world.agentBehaviourOwnsMovement(first)
				&& !world.agentBehaviourOwnsMovement(second)
				&& world.agentBehaviourOwnsMovement(unaffected),
				"Module failure did not disable exactly its affected instances");
			digest << diagnostics[0].agent.value << ':'
				<< static_cast<unsigned>(diagnostics[0].stage) << '|';
		}

		{
			core::World world("Log suppression", 8, 2);
			auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.finishBuild();
			auto const agent = world.createAgent("Logger", room, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("storms.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, logging,
				registry->lookupAgentBehaviour(logging)->getRevision(), {})
				&& world.resumeSimulation() && world.advanceTick(),
				"Could not run log-suppression fixture");
			auto messages = core::consumeLogMessages();
			require(messages.size() == 101
				&& messages.back().msg.find("further messages suppressed")
					!= std::string::npos,
				"Log storm did not produce exactly one suppression summary");
			require(world.advanceTicks(600) && world.advanceTick(),
				"Log-window fixture did not reach its next window");
			messages = core::consumeLogMessages();
			require(messages.size() == 1 && messages[0].msg == "new-window",
				"World log allowance did not reset after 600 ticks");
			digest << "logs:101:1|";
		}
		return digest.str();
	}

	void boundedStormsAndFailuresReplayDeterministically()
	{
		auto const first = runBoundedStormAndFailureFixture();
		auto const second = runBoundedStormAndFailureFixture();
		require(first == second,
			"Repeated headless storm/failure fixtures changed ordering or diagnostics");
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
	boundedStormsAndFailuresReplayDeterministically();
	registryRetainsLoadedAndErrorStatus();
}
