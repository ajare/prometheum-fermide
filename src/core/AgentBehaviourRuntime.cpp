#include "core/AgentBehaviourRuntime.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <sol/sol.hpp>

namespace core
{
	namespace
	{
		struct ScratchBudget
		{
			size_t bytesUsed{ 0 };
			size_t byteLimit{ AgentBehaviourRuntimeAdapter::PreflightMemoryBudgetBytes };
			uint32_t instructionsRemaining{
				AgentBehaviourRuntimeAdapter::PreflightInstructionBudget };
		};

		struct StateCloser
		{
			void operator()(lua_State* state) const noexcept
			{
				if (state) lua_close(state);
			}
		};

		struct HostLoader
		{
			std::string packageName;
			int hostModuleReference{ LUA_NOREF };
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
				return nullptr;
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
			constexpr uint32_t HookInterval{ 1'000 };
			if (budget.instructionsRemaining <= HookInterval)
			{
				budget.instructionsRemaining = 0;
				luaL_error(state, "Agent behaviour preflight instruction budget exceeded");
				return;
			}
			budget.instructionsRemaining -= HookInterval;
		}

		void beginInstructionBudget(lua_State* state, ScratchBudget& budget)
		{
			budget.instructionsRemaining
				= AgentBehaviourRuntimeAdapter::PreflightInstructionBudget;
			lua_sethook(state, instructionHook, LUA_MASKCOUNT, 1'000);
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
			return luaL_error(state, "prometheum.v1 is immutable");
		}

		int requireHostModule(lua_State* state)
		{
			auto& loader = *static_cast<HostLoader*>(
			lua_touserdata(state, lua_upvalueindex(1)));
			auto const* requested = luaL_checkstring(state, 1);
			if (std::string_view(requested) != "prometheum.v1")
			{
				return luaL_error(state,
					"module '%s' is not available in Agent behaviour package '%s'",
					requested, loader.packageName.c_str());
			}
			lua_rawgeti(state, LUA_REGISTRYINDEX, loader.hostModuleReference);
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

			lua_newtable(state);
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

		void openScratchLibraries(sol::state_view lua, HostLoader& loader)
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
			lua_pushcclosure(state, requireHostModule, 1);
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

		AgentBehaviourModulePreflight failure(std::string_view packageName,
			std::string_view moduleName, std::string traceback,
			std::string_view summary = {})
		{
			auto const chunkName = std::format("{}/{}", packageName, moduleName);
			auto const line = diagnosticLine(traceback, chunkName);
			AgentBehaviourModulePreflight result;
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
		std::string_view source)
	{
		auto const normalizedPackage = packageName.empty()
			? std::string("<unknown package>") : std::string(packageName);
		auto const normalizedModule = moduleName.empty()
			? std::string("<unknown module>") : std::string(moduleName);
		auto const chunkName = normalizedPackage + "/" + normalizedModule;
		if (!source.empty() && static_cast<unsigned char>(source.front()) == 0x1b)
		{
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: precompiled Lua bytecode is not accepted",
				"precompiled Lua bytecode is not accepted; use text source");
		}

		ScratchBudget budget;
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
			HostLoader loader{ normalizedPackage };
			openScratchLibraries(lua, loader);
			auto errorHandler = lua["__prometheum_traceback"];

			auto loaded = lua.load_buffer(source.data(), source.size(), "@" + chunkName,
				sol::load_mode::text);
			if (!loaded.valid())
			{
				sol::error error = loaded;
				return failure(normalizedPackage, normalizedModule, error.what());
			}

			sol::protected_function moduleChunk = loaded;
			moduleChunk.set_error_handler(errorHandler);
			beginInstructionBudget(state, budget);
			auto moduleResult = moduleChunk();
			endInstructionBudget(state);
			if (!moduleResult.valid())
			{
				sol::error error = moduleResult;
				return failure(normalizedPackage, normalizedModule, error.what());
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
				return failure(normalizedPackage, normalizedModule, error.what());
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
				chunkName + ":1: " + error.what(), error.what());
		}
	}
}
