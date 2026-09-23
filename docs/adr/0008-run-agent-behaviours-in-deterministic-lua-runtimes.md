# Run Agent behaviours in deterministic per-World Lua runtimes

Status: accepted

Agents may be assigned reusable Lua Agent behaviours from an external Agent behaviour registry, with typed per-Agent configuration and private per-Agent behaviour state. Each World owns one sandboxed Lua 5.4 runtime through a sol2 adapter; behaviour callbacks run synchronously in stable Agent and event order at fixed-tick boundaries, observe immutable semantic values, and enqueue validated commands through the World facade. Lua never receives mutable domain objects, paths, graph vertices, traversal coordination internals, filesystem or process access, and live Lua state is never persisted. This preserves deterministic simulation, keeps the scripting boundary versionable, and prevents scripts from bypassing World validation.

## Considered options

- **Bind `World`, `Agent`, `Graph`, and `Path` directly.** Rejected because scripts could bypass validation, retain unsafe object references across topology rebuilds, and become coupled to simulation internals.
- **Use one process-global Lua state.** Rejected because globals, failures, resource use, and registry packages would leak between Worlds.
- **Share one executed Lua module between Agent instances.** Rejected because mutable module upvalues would silently become shared Agent state. Source or compiled chunks may be cached, but each instance executes its module graph in its own environment.
- **Run a callback every tick.** Rejected as unnecessary polling. Behaviours react to semantic events, deterministic timers, startup, route loss, and teardown callbacks.
- **Serialize the Lua VM or arbitrary instance tables.** Rejected because closures, userdata, cycles, coroutines, and implementation-version details make that state unsafe and unstable. Reset, load, undo, and reload recreate instances from authored configuration.
- **Use unrestricted `require` and standard libraries.** Rejected because package searchers, native modules, I/O, processes, wall time, and debug access violate the trust and determinism boundaries. A custom loader admits only the versioned host API and manifest-declared registry-local modules.

## Consequences

Agent behaviours exclusively own movement intent while enabled and may target only named Markers with stable World-owned identity. The runtime exposes a versioned capability API, semantic Agent events, tick-based timers, and independent deterministic random streams; commands are applied at the next safe tick boundary. Registry schemas and Agent configuration remain ordinary validated C++ authored data, while source lives in managed registry-local Lua files. Behaviour assignment, configuration, reload, registry replacement, and dependent deletion are paused-only coordinated edits. Script errors or resource-budget exhaustion disable the affected instance or module, publish a structured diagnostic, and pause interactive simulation; headless execution fails. Lua and sol2 remain hidden behind the runtime adapter rather than entering domain headers.
