#include "core/AgentBehaviourRuntime.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <sol/sol.hpp>

#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/Building.h"
#include "core/Simulation.h"

namespace core
{
	namespace
	{
		struct ScratchBudget
		{
			size_t bytesUsed{ 0 };
			size_t byteLimit{ AgentBehaviourRuntimeAdapter::DefaultMemoryBudgetBytes };
			uint32_t instructionLimit{
				AgentBehaviourRuntimeAdapter::DefaultInstructionBudget };
			uint32_t instructionsRemaining{
				AgentBehaviourRuntimeAdapter::DefaultInstructionBudget };
			uint32_t hookInterval{ 1'000 };
			bool memoryLimitExceeded{ false };
			bool instructionLimitExceeded{ false };

			explicit ScratchBudget(AgentBehaviourRuntimeLimits limits = {})
				: byteLimit(limits.memoryBytes)
				, instructionLimit(limits.instructionsPerCall)
				, instructionsRemaining(limits.instructionsPerCall)
			{
			}
		};

		bool limitsAreValid(AgentBehaviourRuntimeLimits limits)
		{
			return limits.memoryBytes != 0 && limits.instructionsPerCall != 0;
		}

		struct StateCloser
		{
			void operator()(lua_State* state) const noexcept
			{
				if (state) lua_close(state);
			}
		};

		void pushPrivateEnvironment(lua_State* state);

		struct ModuleLoader
		{
			std::string packageName;
			int hostModuleReference{ LUA_NOREF };
			int environmentReference{ LUA_NOREF };
			std::map<std::string, AgentBehaviourHelperSource> modules;
			std::map<std::string, int> loadedModules;
			std::vector<std::string> dependencyChain;

			void release(lua_State* state)
			{
				for (auto const& [name, reference] : loadedModules)
				{
					(void)name;
					luaL_unref(state, LUA_REGISTRYINDEX, reference);
				}
				loadedModules.clear();
				dependencyChain.clear();
			}
		};

		void* budgetedAllocate(void* userData, void* pointer, size_t oldSize,
			size_t newSize) noexcept
		{
			auto& budget = *static_cast<ScratchBudget*>(userData);
			// For a new block Lua passes a type tag rather than an allocation size
			// in oldSize; only an existing pointer makes that value chargeable.
			if (!pointer) oldSize = 0;
			if (newSize == 0)
			{
				std::free(pointer);
				budget.bytesUsed -= std::min(budget.bytesUsed, oldSize);
				return nullptr;
			}
			auto const growth = newSize > oldSize ? newSize - oldSize : 0;
			if (growth > budget.byteLimit - std::min(budget.byteLimit, budget.bytesUsed))
			{
				// A refused growth leaves both the original block and accounting intact,
				// exactly as Lua's allocator contract requires.
				budget.memoryLimitExceeded = true;
				return nullptr;
			}
			void* replacement = std::realloc(pointer, newSize);
			if (!replacement) return nullptr;
			if (newSize >= oldSize) budget.bytesUsed += newSize - oldSize;
			else budget.bytesUsed -= std::min(budget.bytesUsed, oldSize - newSize);
			return replacement;
		}

		void instructionHook(lua_State* state, lua_Debug*)
		{
			void* userData = nullptr;
			(void)lua_getallocf(state, &userData);
			auto& budget = *static_cast<ScratchBudget*>(userData);
			if (budget.instructionsRemaining <= budget.hookInterval)
			{
				budget.instructionsRemaining = 0;
				budget.instructionLimitExceeded = true;
				luaL_error(state, "Agent behaviour instruction budget exceeded");
				return;
			}
			budget.instructionsRemaining -= budget.hookInterval;
		}

		void beginInstructionBudget(lua_State* state, ScratchBudget& budget)
		{
			budget.instructionsRemaining = budget.instructionLimit;
			budget.hookInterval = std::min<uint32_t>(1'000, budget.instructionLimit);
			budget.memoryLimitExceeded = false;
			budget.instructionLimitExceeded = false;
			lua_sethook(state, instructionHook, LUA_MASKCOUNT,
				static_cast<int>(budget.hookInterval));
		}

		void endInstructionBudget(lua_State* state)
		{
			lua_sethook(state, nullptr, 0, 0);
		}

		int tracebackHandler(lua_State* state)
		{
			auto const* message = lua_tostring(state, 1);
			if (message) luaL_traceback(state, state, message, 1);
			else
			{
				lua_pushliteral(state, "Lua error with no string diagnostic");
			}
			return 1;
		}

		int immutableNewIndex(lua_State* state)
		{
			return luaL_error(state, "Agent behaviour exported value is immutable");
		}

		void pushImmutableValue(lua_State* state, int value,
			std::map<void const*, int>& visited)
		{
			value = lua_absindex(state, value);
			if (!lua_istable(state, value))
			{
				lua_pushvalue(state, value);
				return;
			}

			auto const identity = lua_topointer(state, value);
			if (auto found = visited.find(identity); found != visited.end())
			{
				lua_rawgeti(state, LUA_REGISTRYINDEX, found->second);
				return;
			}

			lua_newtable(state);
			auto const backing = lua_gettop(state);
			(void)lua_newuserdatauv(state, 1, 0);
			auto const proxy = lua_gettop(state);
			lua_newtable(state);
			lua_pushvalue(state, backing);
			lua_setfield(state, -2, "__index");
			lua_pushcfunction(state, immutableNewIndex);
			lua_setfield(state, -2, "__newindex");
			lua_pushliteral(state, "immutable");
			lua_setfield(state, -2, "__metatable");
			lua_setmetatable(state, proxy);
			lua_pushvalue(state, proxy);
			visited.emplace(identity, luaL_ref(state, LUA_REGISTRYINDEX));

			lua_pushnil(state);
			while (lua_next(state, value) != 0)
			{
				pushImmutableValue(state, -2, visited);
				pushImmutableValue(state, -2, visited);
				lua_rawset(state, backing);
				lua_pop(state, 1);
			}
			lua_pushvalue(state, proxy);
			lua_remove(state, backing);
			lua_remove(state, backing);
		}

		void makeTopImmutable(lua_State* state)
		{
			std::map<void const*, int> visited;
			pushImmutableValue(state, -1, visited);
			lua_remove(state, -2);
			for (auto const& [identity, reference] : visited)
			{
				(void)identity;
				luaL_unref(state, LUA_REGISTRYINDEX, reference);
			}
		}

		int requireDeclaredModule(lua_State* state)
		{
			auto& loader = *static_cast<ModuleLoader*>(
				lua_touserdata(state, lua_upvalueindex(1)));
			auto const* requestedText = luaL_checkstring(state, 1);
			std::string const requested(requestedText);
			if (requested == "prometheum.v1")
			{
				lua_rawgeti(state, LUA_REGISTRYINDEX, loader.hostModuleReference);
				return 1;
			}
			if (auto loaded = loader.loadedModules.find(requested);
				loaded != loader.loadedModules.end())
			{
				lua_rawgeti(state, LUA_REGISTRYINDEX, loaded->second);
				return 1;
			}
			auto module = loader.modules.find(requested);
			if (module == loader.modules.end())
			{
				return luaL_error(state,
					"module '%s' is not available: it is not declared in Agent behaviour package '%s'",
					requestedText, loader.packageName.c_str());
			}
			auto cycle = std::find(loader.dependencyChain.begin(),
				loader.dependencyChain.end(), requested);
			if (cycle != loader.dependencyChain.end())
			{
				std::string chain;
				for (auto current = loader.dependencyChain.begin();
					current != loader.dependencyChain.end(); ++current)
				{
					if (!chain.empty()) chain += " -> ";
					chain += *current;
				}
				chain += " -> " + requested;
				return luaL_error(state, "Agent behaviour import cycle: %s", chain.c_str());
			}

			auto const& source = module->second.source;
			auto const chunkName = "@" + loader.packageName + "/"
				+ module->second.sourceModulePath;
			if (luaL_loadbufferx(state, source.data(), source.size(),
				chunkName.c_str(), "t") != LUA_OK) return lua_error(state);
			if (loader.environmentReference == LUA_NOREF)
				return luaL_error(state, "Agent behaviour module environment is unavailable");
			lua_rawgeti(state, LUA_REGISTRYINDEX, loader.environmentReference);
			if (!lua_setupvalue(state, -2, 1))
				return luaL_error(state, "Agent behaviour helper module has no environment");

			loader.dependencyChain.push_back(requested);
			auto const status = lua_pcall(state, 0, 1, 0);
			loader.dependencyChain.pop_back();
			if (status != LUA_OK) return lua_error(state);
			if (lua_isnil(state, -1))
			{
				lua_pop(state, 1);
				lua_pushboolean(state, 1);
			}
			makeTopImmutable(state);
			lua_pushvalue(state, -1);
			loader.loadedModules.emplace(requested,
				luaL_ref(state, LUA_REGISTRYINDEX));
			return 1;
		}

		int createImmutableProxy(lua_State* state, bool hostModule)
		{
			lua_newtable(state);
			auto const backing = lua_gettop(state);
			if (hostModule)
			{
				lua_pushinteger(state, AgentBehaviourRuntimeAdapter::HostApiVersion);
				lua_setfield(state, backing, "api_version");
			}

			(void)lua_newuserdatauv(state, 1, 0);
			auto const proxy = lua_gettop(state);
			lua_newtable(state);
			lua_pushvalue(state, backing);
			lua_setfield(state, -2, "__index");
			lua_pushcfunction(state, immutableNewIndex);
			lua_setfield(state, -2, "__newindex");
			lua_pushliteral(state, "locked");
			lua_setfield(state, -2, "__metatable");
			lua_setmetatable(state, proxy);
			lua_remove(state, backing);
			return luaL_ref(state, LUA_REGISTRYINDEX);
		}

		void removeGlobal(lua_State* state, char const* name)
		{
			lua_pushnil(state);
			lua_setglobal(state, name);
		}

		void openScratchLibraries(sol::state_view lua, ModuleLoader& loader)
		{
			lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string,
				sol::lib::math, sol::lib::utf8);
			auto* state = lua.lua_state();

			// The package library is never opened. Replace its require function
			// with the one reserved host-module loader and remove base functions
			// that could load another chunk or bypass the immutable proxy.
			removeGlobal(state, "package");
			removeGlobal(state, "dofile");
			removeGlobal(state, "loadfile");
			removeGlobal(state, "load");
			removeGlobal(state, "rawset");
			removeGlobal(state, "setmetatable");
			lua_getglobal(state, "string");
			if (lua_istable(state, -1))
			{
				lua_pushnil(state);
				lua_setfield(state, -2, "dump");
			}
			lua_pop(state, 1);
			lua_getglobal(state, "math");
			if (lua_istable(state, -1))
			{
				lua_pushnil(state);
				lua_setfield(state, -2, "random");
				lua_pushnil(state);
				lua_setfield(state, -2, "randomseed");
			}
			lua_pop(state, 1);

			loader.hostModuleReference = createImmutableProxy(state, true);
			lua_pushlightuserdata(state, &loader);
			lua_pushcclosure(state, requireDeclaredModule, 1);
			lua_setglobal(state, "require");
			lua_pushcfunction(state, tracebackHandler);
			lua_setglobal(state, "__prometheum_traceback");
		}

		size_t diagnosticLine(std::string_view traceback,
			std::string_view chunkName)
		{
			auto marker = std::string(chunkName) + ":";
			auto position = traceback.find(marker);
			if (position == std::string_view::npos) return 1;
			position += marker.size();
			size_t line = 0;
			bool foundDigit = false;
			while (position < traceback.size() && traceback[position] >= '0'
				&& traceback[position] <= '9')
			{
				foundDigit = true;
				line = line * 10 + static_cast<size_t>(traceback[position] - '0');
				++position;
			}
			return foundDigit && line != 0 ? line : 1;
		}

		AgentBehaviourRuntimeFailure failureKind(ScratchBudget const& budget,
			AgentBehaviourRuntimeFailure fallback = AgentBehaviourRuntimeFailure::LuaError)
		{
			if (budget.instructionLimitExceeded)
				return AgentBehaviourRuntimeFailure::InstructionBudgetExceeded;
			if (budget.memoryLimitExceeded)
				return AgentBehaviourRuntimeFailure::MemoryBudgetExceeded;
			return fallback;
		}

		AgentBehaviourModulePreflight failure(std::string_view packageName,
			std::string_view moduleName, std::string traceback,
			std::string_view summary = {},
			AgentBehaviourRuntimeFailure kind = AgentBehaviourRuntimeFailure::LuaError)
		{
			auto const chunkName = std::format("{}/{}", packageName, moduleName);
			auto const line = diagnosticLine(traceback, chunkName);
			AgentBehaviourModulePreflight result;
			result.failure = kind;
			result.diagnostic = std::format("Agent behaviour package '{}', module '{}', line {}: {}",
				packageName, moduleName, line,
				summary.empty() ? std::string_view(traceback) : summary);
			result.traceback = std::move(traceback);
			return result;
		}

		bool isCallback(sol::object const& object)
		{
			return object.get_type() == sol::type::nil
				|| object.get_type() == sol::type::function;
		}
	}

	AgentBehaviourModulePreflight AgentBehaviourRuntimeAdapter::preflightModule(
		std::string_view packageName, std::string_view moduleName,
		std::string_view source,
		std::vector<AgentBehaviourHelperSource> const& helpers,
		AgentBehaviourRuntimeLimits limits)
	{
		auto const normalizedPackage = packageName.empty()
			? std::string("<unknown package>") : std::string(packageName);
		auto const normalizedModule = moduleName.empty()
			? std::string("<unknown module>") : std::string(moduleName);
		auto const chunkName = normalizedPackage + "/" + normalizedModule;
		if (!limitsAreValid(limits))
		{
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: Lua runtime limits must be nonzero", {},
				AgentBehaviourRuntimeFailure::ConversionError);
		}
		if (!source.empty() && static_cast<unsigned char>(source.front()) == 0x1b)
		{
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: precompiled Lua bytecode is not accepted",
				"precompiled Lua bytecode is not accepted; use text source");
		}

		ScratchBudget budget(limits);
		std::unique_ptr<lua_State, StateCloser> ownedState(
			lua_newstate(budgetedAllocate, &budget));
		if (!ownedState)
		{
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: could not create the budgeted Lua scratch state");
		}

		try
		{
			auto* state = ownedState.get();
			sol::state_view lua(state);
			ModuleLoader loader;
			loader.packageName = normalizedPackage;
			for (auto const& helper : helpers)
				loader.modules.emplace(helper.name, helper);
			openScratchLibraries(lua, loader);
			pushPrivateEnvironment(state);
			auto const environment = lua_gettop(state);
			lua_pushlightuserdata(state, &loader);
			lua_pushcclosure(state, requireDeclaredModule, 1);
			lua_setfield(state, environment, "require");
			lua_pushvalue(state, environment);
			loader.environmentReference = luaL_ref(state, LUA_REGISTRYINDEX);
			lua_pop(state, 1);
			loader.dependencyChain.push_back(normalizedModule);
			auto errorHandler = lua["__prometheum_traceback"];

			auto loaded = lua.load_buffer(source.data(), source.size(), "@" + chunkName,
				sol::load_mode::text);
			if (!loaded.valid())
			{
				sol::error error = loaded;
				return failure(normalizedPackage, normalizedModule, error.what(), {},
					failureKind(budget));
			}

			sol::protected_function moduleChunk = loaded;
			moduleChunk.push();
			lua_rawgeti(state, LUA_REGISTRYINDEX, loader.environmentReference);
			if (!lua_setupvalue(state, -2, 1))
			{
				lua_pop(state, 1);
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: module has no isolated environment");
			}
			lua_pop(state, 1);
			moduleChunk.set_error_handler(errorHandler);
			beginInstructionBudget(state, budget);
			auto moduleResult = moduleChunk();
			endInstructionBudget(state);
			loader.dependencyChain.clear();
			if (!moduleResult.valid())
			{
				sol::error error = moduleResult;
				return failure(normalizedPackage, normalizedModule, error.what(), {},
					failureKind(budget));
			}
			sol::object exports = moduleResult.get<sol::object>();
			if (exports.get_type() != sol::type::table)
			{
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: module must return a contract table",
					"module must return a contract table");
			}

			auto contract = exports.as<sol::table>();
			auto apiVersion = contract.raw_get<sol::object>("api_version");
			if (!apiVersion.is<lua_Integer>()
				|| apiVersion.as<lua_Integer>() != HostApiVersion)
			{
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: module must declare api_version = 1",
					"module must declare API version 1 as api_version = 1");
			}
			auto factoryObject = contract.raw_get<sol::object>("factory");
			if (factoryObject.get_type() != sol::type::function)
			{
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: module contract is missing its factory",
					"module contract must provide a factory function");
			}

			auto const configurationReference = createImmutableProxy(state, false);
			lua_rawgeti(state, LUA_REGISTRYINDEX, configurationReference);
			sol::object configuration = sol::stack::get<sol::object>(state, -1);
			lua_pop(state, 1);
			sol::protected_function factory = factoryObject.as<sol::protected_function>();
			factory.set_error_handler(errorHandler);
			beginInstructionBudget(state, budget);
			auto factoryResult = factory(configuration);
			endInstructionBudget(state);
			luaL_unref(state, LUA_REGISTRYINDEX, configurationReference);
			if (!factoryResult.valid())
			{
				sol::error error = factoryResult;
				return failure(normalizedPackage, normalizedModule, error.what(), {},
					failureKind(budget));
			}
			sol::object instanceObject = factoryResult.get<sol::object>();
			if (instanceObject.get_type() != sol::type::table)
			{
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: factory must return an instance table",
					"factory must return an instance table");
			}

			auto instance = instanceObject.as<sol::table>();
			for (auto const* callback : { "on_start", "on_event", "on_timer",
				"on_route_lost", "on_stop" })
			{
				auto value = instance.raw_get<sol::object>(callback);
				if (!isCallback(value))
				{
					return failure(normalizedPackage, normalizedModule,
						chunkName + ":1: invalid instance callback",
						std::format("instance field '{}' must be a function when present",
							callback));
				}
			}

			AgentBehaviourModulePreflight result;
			result.loaded = true;
			return result;
		}
		catch (std::exception const& error)
		{
			endInstructionBudget(ownedState.get());
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: " + error.what(), error.what(),
				failureKind(budget, AgentBehaviourRuntimeFailure::ConversionError));
		}
		catch (...)
		{
			endInstructionBudget(ownedState.get());
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: unknown sol2 conversion failure",
				"unknown sol2 conversion failure",
				failureKind(budget, AgentBehaviourRuntimeFailure::ConversionError));
		}
	}

	AgentBehaviourModulePreflight AgentBehaviourRuntimeAdapter::preflightHelperModule(
		std::string_view packageName, std::string_view helperName,
		std::string_view moduleName, std::string_view source,
		std::vector<AgentBehaviourHelperSource> const& helpers,
		AgentBehaviourRuntimeLimits limits)
	{
		std::vector<AgentBehaviourHelperSource> graph = helpers;
		auto found = std::find_if(graph.begin(), graph.end(),
			[helperName](AgentBehaviourHelperSource const& candidate)
			{ return candidate.name == helperName; });
		if (found == graph.end())
			graph.push_back({ std::string(helperName), std::string(moduleName),
				std::string(source) });
		else
		{
			found->sourceModulePath = moduleName;
			found->source = source;
		}
		// Helper roots have no behaviour contract. A tiny text root imports the
		// declared module through exactly the same loader and budget used by a
		// behaviour, so syntax, nested imports, cycles, and module execution are
		// validated without inventing a second loading path.
		auto wrapper = std::format(
			"local helper = require(\"{}\")\n"
			"return {{ api_version = 1, factory = function() return {{}} end }}\n",
			helperName);
		return preflightModule(packageName, moduleName, wrapper, graph, limits);
	}

	namespace
	{
		constexpr char MarkerMetatable[] = "prometheum.v1.marker";

		struct MarkerHandle
		{
			MarkerId marker;
		};

		enum class PendingMovementCommandType { MoveTo, Cancel };

		struct PendingMovementCommand
		{
			PendingMovementCommandType type{ PendingMovementCommandType::MoveTo };
			AgentId agent;
			MarkerId marker;
		};

		struct CallbackScope
		{
			bool active{ false };
			bool movementCommandIssued{ false };
			AgentId agent;
			std::function<MovementCommandResult(MarkerId)> inspectMove;
			std::function<MovementCommandResult()> inspectCancel;
			std::vector<PendingMovementCommand> commands;
		};

		void pushImmutableProxy(lua_State* state)
		{
			// The caller leaves a backing table on top. A userdata proxy cannot be
			// altered through raw table operations; reads resolve against the hidden
			// backing table and every assignment reaches __newindex.
			auto const backing = lua_gettop(state);
			(void)lua_newuserdatauv(state, 1, 0);
			auto const proxy = lua_gettop(state);
			lua_newtable(state);
			lua_pushvalue(state, backing);
			lua_setfield(state, -2, "__index");
			lua_pushcfunction(state, immutableNewIndex);
			lua_setfield(state, -2, "__newindex");
			lua_pushliteral(state, "immutable");
			lua_setfield(state, -2, "__metatable");
			lua_setmetatable(state, proxy);
			lua_remove(state, backing);
		}

		void pushCommandResult(lua_State* state, bool accepted, std::string_view status)
		{
			lua_newtable(state);
			lua_pushboolean(state, accepted);
			lua_setfield(state, -2, "accepted");
			lua_pushlstring(state, status.data(), status.size());
			lua_setfield(state, -2, "status");
			pushImmutableProxy(state);
		}

		std::string_view movementStatusName(MovementCommandStatus status)
		{
			switch (status)
			{
			case MovementCommandStatus::Accepted: return "accepted";
			case MovementCommandStatus::NoOp: return "no_op";
			case MovementCommandStatus::UnknownAgent: return "unknown_agent";
			case MovementCommandStatus::InactiveAgent: return "inactive_agent";
			case MovementCommandStatus::UnknownMarker: return "unknown_marker";
			case MovementCommandStatus::AgentBusy: return "agent_busy";
			case MovementCommandStatus::TopologyUnavailable: return "topology_unavailable";
			case MovementCommandStatus::BehaviourOwned: return "behaviour_owned";
			}
			return "unknown";
		}

		std::string_view routeLossReasonName(RouteLossReason reason)
		{
			switch (reason)
			{
			case RouteLossReason::Unreachable: return "unreachable";
			case RouteLossReason::TopologyChanged: return "topology_changed";
			case RouteLossReason::DestinationRemoved: return "destination_removed";
			case RouteLossReason::None: break;
			}
			return "unknown";
		}

		std::string_view cancellationReasonName(MovementCancellationReason reason)
		{
			return reason == MovementCancellationReason::Explicit ? "explicit" : "unknown";
		}

		int markerToString(lua_State* state)
		{
			(void)luaL_checkudata(state, 1, MarkerMetatable);
			lua_pushliteral(state, "Marker");
			return 1;
		}

		int markerEqual(lua_State* state)
		{
			auto* lhs = static_cast<MarkerHandle*>(
				luaL_testudata(state, 1, MarkerMetatable));
			auto* rhs = static_cast<MarkerHandle*>(
				luaL_testudata(state, 2, MarkerMetatable));
			lua_pushboolean(state, lhs && rhs && lhs->marker == rhs->marker);
			return 1;
		}

		void ensureMarkerMetatable(lua_State* state)
		{
			if (luaL_newmetatable(state, MarkerMetatable))
			{
				lua_pushliteral(state, "opaque Marker handle");
				lua_setfield(state, -2, "__metatable");
				lua_pushcfunction(state, markerToString);
				lua_setfield(state, -2, "__tostring");
				lua_pushcfunction(state, markerEqual);
				lua_setfield(state, -2, "__eq");
			}
			lua_pop(state, 1);
		}

		void pushMarkerHandle(lua_State* state, MarkerId marker)
		{
			auto* handle = static_cast<MarkerHandle*>(
				lua_newuserdatauv(state, sizeof(MarkerHandle), 0));
			*handle = MarkerHandle{ marker };
			luaL_setmetatable(state, MarkerMetatable);
		}

		CallbackScope* activeScope(lua_State* state)
		{
			auto* scope = static_cast<CallbackScope*>(
				lua_touserdata(state, lua_upvalueindex(1)));
			if (!scope || !scope->active)
			{
				luaL_error(state, "Agent behaviour callback context is no longer active");
				return nullptr;
			}
			if (scope->movementCommandIssued)
			{
				luaL_error(state,
					"multiple movement commands in one Agent behaviour callback are a programming error");
				return nullptr;
			}
			scope->movementCommandIssued = true;
			return scope;
		}

		int queueMoveTo(lua_State* state)
		{
			auto* scope = activeScope(state);
			if (!scope) return 0;

			MarkerHandle* handle = nullptr;
			for (int index = 1; index <= lua_gettop(state) && !handle; ++index)
				handle = static_cast<MarkerHandle*>(
					luaL_testudata(state, index, MarkerMetatable));
			if (!handle)
			{
				pushCommandResult(state, false, "invalid_marker");
				return 1;
			}

			auto const result = scope->inspectMove(handle->marker);
			if (result.status == MovementCommandStatus::Accepted)
				scope->commands.push_back({ PendingMovementCommandType::MoveTo,
					scope->agent, handle->marker });
			pushCommandResult(state, result.accepted(), movementStatusName(result.status));
			return 1;
		}

		int queueCancelMovement(lua_State* state)
		{
			auto* scope = activeScope(state);
			if (!scope) return 0;
			auto const result = scope->inspectCancel();
			if (result.status == MovementCommandStatus::Accepted)
				scope->commands.push_back({ PendingMovementCommandType::Cancel,
					scope->agent, {} });
			pushCommandResult(state, result.accepted(), movementStatusName(result.status));
			return 1;
		}

		struct ProtectedCallResult
		{
			bool succeeded{ false };
			AgentBehaviourRuntimeFailure failure{ AgentBehaviourRuntimeFailure::None };
			std::string diagnostic;
			std::string traceback;
		};

		ProtectedCallResult protectedCall(lua_State* state, ScratchBudget& budget,
			int argumentCount, int resultCount)
		{
			auto const functionIndex = lua_gettop(state) - argumentCount;
			lua_getglobal(state, "__prometheum_traceback");
			lua_insert(state, functionIndex);
			beginInstructionBudget(state, budget);
			auto const status = lua_pcall(state, argumentCount, resultCount,
				functionIndex);
			endInstructionBudget(state);
			if (status != LUA_OK)
			{
				auto const* message = lua_tostring(state, -1);
				ProtectedCallResult result;
				result.failure = status == LUA_ERRMEM
					? AgentBehaviourRuntimeFailure::MemoryBudgetExceeded
					: failureKind(budget);
				result.diagnostic = message ? message : "Lua execution failed";
				result.traceback = result.diagnostic;
				lua_pop(state, 1);
				lua_remove(state, functionIndex);
				return result;
			}
			lua_remove(state, functionIndex);
			ProtectedCallResult result;
			result.succeeded = true;
			return result;
		}

		void copyGlobal(lua_State* state, int environment, char const* name)
		{
			lua_getglobal(state, name);
			lua_setfield(state, environment, name);
		}

		void copyLibrary(lua_State* state, int environment, char const* name)
		{
			lua_getglobal(state, name);
			if (!lua_istable(state, -1))
			{
				lua_pop(state, 1);
				return;
			}
			lua_newtable(state);
			auto const copy = lua_gettop(state);
			lua_pushnil(state);
			while (lua_next(state, -3) != 0)
			{
				lua_pushvalue(state, -2);
				lua_pushvalue(state, -2);
				lua_settable(state, copy);
				lua_pop(state, 1);
			}
			lua_remove(state, copy - 1);
			makeTopImmutable(state);
			lua_setfield(state, environment, name);
		}

		void pushPrivateEnvironment(lua_State* state)
		{
			lua_newtable(state);
			auto const environment = lua_gettop(state);
			for (auto const* name : { "assert", "error", "ipairs", "next", "pairs",
				"pcall", "rawequal", "rawget", "select", "tonumber", "tostring",
				"type", "xpcall", "_VERSION", "require" })
				copyGlobal(state, environment, name);
			for (auto const* name : { "table", "string", "math", "utf8" })
				copyLibrary(state, environment, name);
			lua_pushvalue(state, environment);
			lua_setfield(state, environment, "_G");
		}

		void pushConfiguration(lua_State* state,
			AgentBehaviourConfiguration const& configuration)
		{
			lua_newtable(state);
			for (auto const& [name, value] : configuration)
			{
				std::visit([&](auto const& typed)
				{
					using T = std::decay_t<decltype(typed)>;
					if constexpr (std::is_same_v<T, bool>)
						lua_pushboolean(state, typed);
					else if constexpr (std::is_same_v<T, int64_t>)
						lua_pushinteger(state, static_cast<lua_Integer>(typed));
					else if constexpr (std::is_same_v<T, double>)
						lua_pushnumber(state, typed);
					else if constexpr (std::is_same_v<T, std::string>)
						lua_pushlstring(state, typed.data(), typed.size());
					else if constexpr (std::is_same_v<T, AgentBehaviourDuration>)
						lua_pushinteger(state, static_cast<lua_Integer>(typed.ticks));
					else
						pushMarkerHandle(state, typed);
				}, value);
				lua_setfield(state, -2, name.c_str());
			}
			pushImmutableProxy(state);
		}
	}

	struct AgentBehaviourRuntimeAdapter::Impl
	{
		struct PendingOutcome
		{
			uint64_t sequence{ 0 };
			uint64_t tick{ 0 };
			SimulationEventType type{ SimulationEventType::DestinationReached };
			MarkerId destination;
			RouteLossReason routeLossReason{ RouteLossReason::None };
			MovementCancellationReason cancellationReason{
				MovementCancellationReason::None };
		};

		struct Definition
		{
			AgentId agent;
			AgentBehaviourAssignment assignment;
			std::string registryUuid;
			uint64_t packageRevision{ 0 };
			std::string packageName;
			std::string moduleName;
			std::string source;
			std::vector<AgentBehaviourHelperSource> helpers;
		};

		struct Instance
		{
			AgentBehaviourAssignment assignment;
			std::string registryUuid;
			uint64_t packageRevision{ 0 };
			std::string packageName;
			std::string moduleName;
			int environmentReference{ LUA_NOREF };
			int configurationReference{ LUA_NOREF };
			int instanceReference{ LUA_NOREF };
			std::unique_ptr<ModuleLoader> moduleLoader;
			bool started{ false };
			bool disabled{ false };
			CallbackScope scope;
			std::vector<PendingOutcome> outcomes;
		};

		ScratchBudget budget;
		std::unique_ptr<lua_State, StateCloser> state;
		ModuleLoader hostLoader;
		std::map<AgentId, Instance> instances;
		std::vector<AgentBehaviourRuntimeDiagnostic> diagnostics;
		uint64_t observedOutcomeCount{ 0 };
		uint64_t lastObservedSequence{ 0 };

		explicit Impl(AgentBehaviourRuntimeLimits limits)
			: budget(limits)
			, state(lua_newstate(budgetedAllocate, &budget))
		{
			if (!state) throw std::runtime_error("Could not create Building Lua runtime");
			sol::state_view lua(state.get());
			hostLoader.packageName = "Building Agent behaviours";
			openScratchLibraries(lua, hostLoader);
			ensureMarkerMetatable(state.get());
		}

		void release(Instance& instance)
		{
			if (instance.moduleLoader) instance.moduleLoader->release(state.get());
			luaL_unref(state.get(), LUA_REGISTRYINDEX, instance.instanceReference);
			luaL_unref(state.get(), LUA_REGISTRYINDEX, instance.configurationReference);
			luaL_unref(state.get(), LUA_REGISTRYINDEX, instance.environmentReference);
			instance.instanceReference = LUA_NOREF;
			instance.configurationReference = LUA_NOREF;
			instance.environmentReference = LUA_NOREF;
			instance.moduleLoader.reset();
		}

		void record(Definition const& definition, AgentBehaviourRuntimeStage stage,
			std::string_view callback, ProtectedCallResult const& result)
		{
			diagnostics.push_back({ result.failure, stage, definition.agent,
				definition.packageName, definition.moduleName, std::string(callback),
				result.diagnostic, result.traceback });
		}

		void record(Instance const& instance, AgentBehaviourRuntimeStage stage,
			std::string_view callback, ProtectedCallResult const& result)
		{
			diagnostics.push_back({ result.failure, stage, instance.scope.agent,
				instance.packageName, instance.moduleName, std::string(callback),
				result.diagnostic, result.traceback });
		}

		void clear()
		{
			for (auto& [agent, instance] : instances)
			{
				(void)agent;
				release(instance);
			}
			instances.clear();
			diagnostics.clear();
			observedOutcomeCount = 0;
			lastObservedSequence = 0;
			(void)lua_gc(state.get(), LUA_GCCOLLECT);
		}

		bool construct(Definition const& definition, Instance& instance)
		{
			auto* lua = state.get();
			auto const base = lua_gettop(lua);
			auto conversionFailure = [&](AgentBehaviourRuntimeStage stage,
				std::string message)
			{
				ProtectedCallResult result;
				result.failure = AgentBehaviourRuntimeFailure::ConversionError;
				result.diagnostic = std::move(message);
				result.traceback = result.diagnostic;
				record(definition, stage, {}, result);
				lua_settop(lua, base);
				return false;
			};
			auto const chunkName = "@" + definition.packageName + "/"
				+ definition.moduleName;
			budget.memoryLimitExceeded = false;
			auto const loadStatus = luaL_loadbufferx(lua, definition.source.data(),
				definition.source.size(), chunkName.c_str(), "t");
			if (loadStatus != LUA_OK)
			{
				ProtectedCallResult result;
				result.failure = loadStatus == LUA_ERRMEM || budget.memoryLimitExceeded
					? AgentBehaviourRuntimeFailure::MemoryBudgetExceeded
					: AgentBehaviourRuntimeFailure::LuaError;
				auto const* message = lua_tostring(lua, -1);
				result.diagnostic = message ? message : "Lua module load failed";
				result.traceback = result.diagnostic;
				record(definition, AgentBehaviourRuntimeStage::ModuleLoad, {}, result);
				lua_settop(lua, base);
				return false;
			}
			auto const chunk = lua_gettop(lua);

			pushPrivateEnvironment(lua);
			auto const environment = lua_gettop(lua);
			lua_pushvalue(lua, environment);
			instance.environmentReference = luaL_ref(lua, LUA_REGISTRYINDEX);
			instance.moduleLoader = std::make_unique<ModuleLoader>();
			instance.moduleLoader->packageName = definition.packageName;
			instance.moduleLoader->hostModuleReference = hostLoader.hostModuleReference;
			instance.moduleLoader->environmentReference = instance.environmentReference;
			for (auto const& helper : definition.helpers)
				instance.moduleLoader->modules.emplace(helper.name, helper);
			lua_pushlightuserdata(lua, instance.moduleLoader.get());
			lua_pushcclosure(lua, requireDeclaredModule, 1);
			lua_setfield(lua, environment, "require");
			lua_pushvalue(lua, environment);
			if (!lua_setupvalue(lua, chunk, 1))
				return conversionFailure(AgentBehaviourRuntimeStage::ModuleLoad,
					"Agent behaviour module has no isolated environment");
			lua_remove(lua, environment);

			instance.moduleLoader->dependencyChain.push_back(definition.moduleName);
			auto const moduleLoaded = protectedCall(lua, budget, 0, 1);
			instance.moduleLoader->dependencyChain.clear();
			if (!moduleLoaded.succeeded)
			{
				record(definition, AgentBehaviourRuntimeStage::ModuleLoad, {}, moduleLoaded);
				lua_settop(lua, base);
				return false;
			}
			if (!lua_istable(lua, -1))
				return conversionFailure(AgentBehaviourRuntimeStage::ModuleLoad,
					"Agent behaviour module must return a contract table");
			auto const contract = lua_gettop(lua);
			lua_pushliteral(lua, "api_version");
			lua_rawget(lua, contract);
			auto const apiVersion = lua_isinteger(lua, -1) ? lua_tointeger(lua, -1) : 0;
			lua_pop(lua, 1);
			if (apiVersion != HostApiVersion)
				return conversionFailure(AgentBehaviourRuntimeStage::ModuleLoad,
					"Agent behaviour module API version is invalid");
			lua_pushliteral(lua, "factory");
			lua_rawget(lua, contract);
			if (!lua_isfunction(lua, -1))
				return conversionFailure(AgentBehaviourRuntimeStage::Factory,
					"Agent behaviour module factory is invalid");

			pushConfiguration(lua, definition.assignment.configuration);
			lua_pushvalue(lua, -1);
			instance.configurationReference = luaL_ref(lua, LUA_REGISTRYINDEX);
			auto const factoryCalled = protectedCall(lua, budget, 1, 1);
			if (!factoryCalled.succeeded)
			{
				record(definition, AgentBehaviourRuntimeStage::Factory, {}, factoryCalled);
				lua_settop(lua, base);
				return false;
			}
			if (!lua_istable(lua, -1))
				return conversionFailure(AgentBehaviourRuntimeStage::Factory,
					"Agent behaviour factory must return an instance table");
			instance.instanceReference = luaL_ref(lua, LUA_REGISTRYINDEX);
			lua_settop(lua, base);
			return true;
		}

		void synchronize(std::vector<Definition> const& definitions)
		{
			std::map<AgentId, Definition const*> desired;
			for (auto const& definition : definitions)
				desired.emplace(definition.agent, &definition);

			for (auto iterator = instances.begin(); iterator != instances.end();)
			{
				auto found = desired.find(iterator->first);
				if (found != desired.end()
					&& iterator->second.assignment == found->second->assignment
					&& iterator->second.registryUuid == found->second->registryUuid
					&& iterator->second.packageRevision == found->second->packageRevision)
				{
					++iterator;
					continue;
				}
				release(iterator->second);
				iterator = instances.erase(iterator);
			}

			for (auto const& definition : definitions)
			{
				if (instances.contains(definition.agent)) continue;
				Instance instance;
				instance.assignment = definition.assignment;
				instance.registryUuid = definition.registryUuid;
				instance.packageRevision = definition.packageRevision;
				instance.packageName = definition.packageName;
				instance.moduleName = definition.moduleName;
				instance.scope.agent = definition.agent;
				if (!construct(definition, instance))
				{
					instance.disabled = true;
					release(instance);
					(void)lua_gc(state.get(), LUA_GCCOLLECT);
				}
				instances.emplace(definition.agent, std::move(instance));
			}
		}

		void pushContext(Instance& instance)
		{
			auto* lua = state.get();
			lua_newtable(lua);
			lua_rawgeti(lua, LUA_REGISTRYINDEX, instance.configurationReference);
			lua_setfield(lua, -2, "configuration");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, queueMoveTo, 1);
			lua_setfield(lua, -2, "move_to");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, queueCancelMovement, 1);
			lua_setfield(lua, -2, "cancel_movement");
			pushImmutableProxy(lua);
		}

		void prepareScope(Building& building, AgentId agentId, Instance& instance,
			std::vector<PendingMovementCommand> const& pendingCommands)
		{
			instance.scope.active = true;
			instance.scope.movementCommandIssued = false;
			instance.scope.commands.clear();
			instance.scope.inspectMove = [&building, agentId, &pendingCommands](MarkerId marker)
			{
				auto pending = std::find_if(pendingCommands.rbegin(), pendingCommands.rend(),
					[agentId](PendingMovementCommand const& command)
					{ return command.agent == agentId; });
				if (pending != pendingCommands.rend())
					return MovementCommandResult{ pending->type == PendingMovementCommandType::MoveTo
						&& pending->marker == marker ? MovementCommandStatus::NoOp
						: MovementCommandStatus::AgentBusy };
				return building.inspectBehaviourMoveToMarker(agentId, marker);
			};
			instance.scope.inspectCancel = [&building, agentId, &pendingCommands]
			{
				auto pending = std::find_if(pendingCommands.rbegin(), pendingCommands.rend(),
					[agentId](PendingMovementCommand const& command)
					{ return command.agent == agentId; });
				if (pending != pendingCommands.rend())
					return MovementCommandResult{ pending->type == PendingMovementCommandType::Cancel
						? MovementCommandStatus::NoOp : MovementCommandStatus::AgentBusy };
				return building.inspectBehaviourMovementCancellation(agentId);
			};
		}

		bool finishCallback(Instance& instance, std::string_view callback,
			ProtectedCallResult const& result,
			std::vector<PendingMovementCommand>& commands)
		{
			instance.scope.active = false;
			if (result.succeeded)
				commands.insert(commands.end(), instance.scope.commands.begin(),
					instance.scope.commands.end());
			else
			{
				instance.disabled = true;
				record(instance, AgentBehaviourRuntimeStage::Callback, callback, result);
			}
			instance.scope.commands.clear();
			return result.succeeded;
		}

		bool pushCallback(Instance& instance, char const* callback)
		{
			auto* lua = state.get();
			lua_rawgeti(lua, LUA_REGISTRYINDEX, instance.instanceReference);
			lua_pushstring(lua, callback);
			lua_rawget(lua, -2);
			lua_remove(lua, -2);
			if (lua_isnil(lua, -1))
			{
				lua_pop(lua, 1);
				return false;
			}
			return lua_isfunction(lua, -1);
		}

		void pushSemanticEvent(PendingOutcome const& outcome)
		{
			auto* lua = state.get();
			lua_newtable(lua);
			auto const backing = lua_gettop(lua);
			auto const type = outcome.type == SimulationEventType::DestinationReached
				? std::string_view("destination_reached")
				: std::string_view("movement_cancelled");
			lua_pushlstring(lua, type.data(), type.size());
			lua_setfield(lua, backing, "type");
			lua_pushinteger(lua, static_cast<lua_Integer>(outcome.tick));
			lua_setfield(lua, backing, "tick");
			lua_pushinteger(lua, static_cast<lua_Integer>(outcome.sequence));
			lua_setfield(lua, backing, "sequence");
			pushMarkerHandle(lua, outcome.destination);
			lua_setfield(lua, backing, "destination");
			if (outcome.type == SimulationEventType::MovementCancelled)
			{
				auto const reason = cancellationReasonName(outcome.cancellationReason);
				lua_pushlstring(lua, reason.data(), reason.size());
				lua_setfield(lua, backing, "reason");
			}
			pushImmutableProxy(lua);
		}

		void runBoundaryCallbacks(Building& building)
		{
			std::vector<PendingMovementCommand> commands;
			std::vector<AgentId> disabledAgents;
			for (auto& [agentId, instance] : instances)
			{
				if (instance.disabled) continue;
				auto agent = building.mAgents.find(agentId);
				if (!agent || !agent->isActive()) continue;
				auto* lua = state.get();

				if (!instance.started)
				{
					instance.started = true;
					auto const base = lua_gettop(lua);
					if (pushCallback(instance, "on_start"))
					{
						prepareScope(building, agentId, instance, commands);
						pushContext(instance);
						lua_rawgeti(lua, LUA_REGISTRYINDEX,
							instance.configurationReference);
						auto const result = protectedCall(lua, budget, 2, 0);
						finishCallback(instance, "on_start", result, commands);
					}
					lua_settop(lua, base);
				}

				if (instance.disabled)
				{
					instance.outcomes.clear();
					release(instance);
					(void)lua_gc(state.get(), LUA_GCCOLLECT);
					disabledAgents.push_back(agentId);
					continue;
				}

				std::sort(instance.outcomes.begin(), instance.outcomes.end(),
					[](PendingOutcome const& lhs, PendingOutcome const& rhs)
					{ return lhs.sequence < rhs.sequence; });
				for (auto const& outcome : instance.outcomes)
				{
					auto const base = lua_gettop(lua);
					if (outcome.type == SimulationEventType::RouteLost)
					{
						if (pushCallback(instance, "on_route_lost"))
						{
							prepareScope(building, agentId, instance, commands);
							pushMarkerHandle(lua, outcome.destination);
							auto const reason = routeLossReasonName(outcome.routeLossReason);
							lua_pushlstring(lua, reason.data(), reason.size());
							pushContext(instance);
							auto const result = protectedCall(lua, budget, 3, 0);
							finishCallback(instance, "on_route_lost", result, commands);
						}
					}
					else if (pushCallback(instance, "on_event"))
					{
						prepareScope(building, agentId, instance, commands);
						pushSemanticEvent(outcome);
						pushContext(instance);
						auto const result = protectedCall(lua, budget, 2, 0);
						finishCallback(instance, "on_event", result, commands);
					}
					lua_settop(lua, base);
					if (instance.disabled) break;
				}
				instance.outcomes.clear();
				if (instance.disabled)
				{
					release(instance);
					(void)lua_gc(state.get(), LUA_GCCOLLECT);
					disabledAgents.push_back(agentId);
				}
			}

			// Every callback above has returned and the phase marker is still None.
			// Apply only complete successful callback batches, in stable Agent/event
			// order, through the same validated movement seam as the C++ facade.
			for (auto const& command : commands)
			{
				if (std::find(disabledAgents.begin(), disabledAgents.end(), command.agent)
					!= disabledAgents.end()) continue;
				if (command.type == PendingMovementCommandType::MoveTo)
					(void)building.moveBehaviourAgentToMarker(
						command.agent, command.marker);
				else
					(void)building.cancelBehaviourAgentMovement(command.agent);
			}
			for (auto agent : disabledAgents)
				(void)building.cancelBehaviourAgentMovement(agent);
		}
	};

	AgentBehaviourRuntimeAdapter::AgentBehaviourRuntimeAdapter(
		AgentBehaviourRuntimeLimits limits)
	{
		if (!limitsAreValid(limits))
			throw std::invalid_argument("Agent behaviour runtime limits must be nonzero");
		mImpl = std::make_unique<Impl>(limits);
	}

	AgentBehaviourRuntimeAdapter::~AgentBehaviourRuntimeAdapter() = default;

	void AgentBehaviourRuntimeAdapter::runBoundary(Building& building)
	{
		if (building.mCurrentPhase != SimulationPhase::None) return;
		std::vector<Impl::Definition> definitions;
		auto const registry = building.mAgentBehaviourRegistry;
		if (registry && registry->mPackageDirectory)
		{
			std::vector<AgentBehaviourHelperSource> helpers;
			helpers.reserve(registry->mHelperModules.size());
			for (auto const& [name, helper] : registry->mHelperModules)
			{
				auto source = registry->mSourceCache.find(helper->getSourceModulePath());
				if (source == registry->mSourceCache.end()) continue;
				helpers.push_back({ name, helper->getSourceModulePath(), source->second });
			}
			for (auto const& [agentId, agent] : building.mAgents.entries())
			{
				if (!agent || !agent->getBehaviourAssignment()) continue;
				auto const& assignment = *agent->getBehaviourAssignment();
				auto const* behaviour = registry->lookupAgentBehaviour(
					assignment.behaviour);
				if (!behaviour || behaviour->getModuleStatus()
					!= AgentBehaviourModuleStatus::Loaded) continue;
				auto source = registry->mSourceCache.find(
					behaviour->getSourceModulePath());
				if (source == registry->mSourceCache.end()) continue;
				definitions.push_back({ agentId, assignment, registry->getUuid(),
					registry->getPackageRevision(),
					registry->mPackageDirectory->filename().string(),
					behaviour->getSourceModulePath(), source->second, helpers });
			}
		}
		try
		{
			mImpl->synchronize(definitions);
			mImpl->runBoundaryCallbacks(building);
		}
		catch (std::exception const& error)
		{
			// sol2 conversions and adapter-side Lua value marshaling are contained at
			// the same boundary as protected Lua errors. The complete failure policy
			// (pause/headless stop and module scope) is layered by ticket #159.
			mImpl->diagnostics.push_back({
				AgentBehaviourRuntimeFailure::ConversionError,
				AgentBehaviourRuntimeStage::Callback, {}, {}, {}, {},
				error.what(), error.what() });
			for (auto& [agent, instance] : mImpl->instances)
			{
				(void)agent;
				if (!instance.scope.active) continue;
				instance.scope.active = false;
				instance.scope.commands.clear();
				instance.disabled = true;
			}
		}
		catch (...)
		{
			mImpl->diagnostics.push_back({
				AgentBehaviourRuntimeFailure::ConversionError,
				AgentBehaviourRuntimeStage::Callback, {}, {}, {}, {},
				"Unknown Lua adapter conversion failure",
				"Unknown Lua adapter conversion failure" });
			for (auto& [agent, instance] : mImpl->instances)
			{
				(void)agent;
				if (!instance.scope.active) continue;
				instance.scope.active = false;
				instance.scope.commands.clear();
				instance.disabled = true;
			}
		}
	}

	void AgentBehaviourRuntimeAdapter::observeOutcome(SimulationEvent const& event)
	{
		if (event.type != SimulationEventType::DestinationReached
			&& event.type != SimulationEventType::MovementCancelled
			&& event.type != SimulationEventType::RouteLost) return;
		auto found = mImpl->instances.find(event.agent.id);
		if (found == mImpl->instances.end() || found->second.disabled) return;
		found->second.outcomes.push_back({ event.sequence, event.tick, event.type,
			event.destinationMarker, event.routeLossReason,
			event.movementCancellationReason });
		++mImpl->observedOutcomeCount;
		mImpl->lastObservedSequence = event.sequence;
	}

	void AgentBehaviourRuntimeAdapter::removeInstance(AgentId agent)
	{
		auto found = mImpl->instances.find(agent);
		if (found == mImpl->instances.end()) return;
		mImpl->release(found->second);
		mImpl->instances.erase(found);
	}

	bool AgentBehaviourRuntimeAdapter::isInstanceDisabled(AgentId agent) const
	{
		auto found = mImpl->instances.find(agent);
		return found != mImpl->instances.end() && found->second.disabled;
	}

	std::vector<AgentBehaviourRuntimeDiagnostic>
	AgentBehaviourRuntimeAdapter::consumeDiagnostics()
	{
		auto result = std::move(mImpl->diagnostics);
		mImpl->diagnostics.clear();
		return result;
	}

	AgentBehaviourRuntimeLimits AgentBehaviourRuntimeAdapter::getLimits() const
	{
		return { mImpl->budget.byteLimit, mImpl->budget.instructionLimit };
	}

	void AgentBehaviourRuntimeAdapter::reset()
	{
		mImpl->clear();
	}
}
