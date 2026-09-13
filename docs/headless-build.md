# Headless simulation smoke scenario

The `core` target builds the shared simulation as `phosphorus-fluoride-core.lib`. Both the `headless` and `imgui` projects reference that static library, so simulation sources are compiled once per configuration instead of being duplicated in each executable. The `headless` target builds deterministic smoke scenarios without SDL, ImGui, OpenGL, rendering, or audio dependencies. The scenarios route agents between marker vertices, verify ordinary request/permit/commit behavior, denial and cancellation, exercise manual, automatic, remote-controlled, unavailable, fair two-sided queued, and wide concurrent doors, verify scoped open leases, sensor-driven reopening, graceful deactivation, queue cancellation, physical waiting separation, logical queue overflow, compatible replan priority, and deterministic permit expiry/reassignment, advance buildings in fixed ticks, and exit unsuccessfully if an invariant fails or two identical runs produce different snapshots or events.

## Deterministic simulation API

`Building::advanceTick()` and `Building::advanceTicks()` are the headless seam. Each tick is `Building::getFixedTimestep()` (1/60 second) and runs these phases in order:

1. resource advancement;
2. intent collection;
3. allocation;
4. movement;
5. commit;
6. cleanup and event publication.

The traversal protocol now uses those seams directly. On reaching an edge, an agent creates one building-owned request during intent collection. Allocation grants an immediate permit for an unconstrained edge, movement advances the agent between the edge's path vertices while it remains a source-sector occupant, commit transfers sector membership at the destination endpoint, and cleanup releases the transaction records. Denied requests remain blocked, and cancelling a path releases its request and permit without committing. Resource updates and stable-ID-ordered agent updates remain separated.

`Building::getSimulationSnapshot()` returns a value snapshot containing the tick and stable agent IDs, sectors, positions, path state, path progress, active locomotion task, traversal request, and traversal permit. It also exposes the building-owned interaction points, device operations, traversal resources, requests, and permits by stable typed ID. Door-resource snapshots include activation mode, enabled state, open state and percentage, typed open-lease counts, presence and obstruction observations, generated queue lanes and their position owners, and every independent crossing lane and owner. Door requests expose separate logical queue tickets, optional physical queue-position reservations, and their typed opening operation. Ladder-resource snapshots expose capacity derived from usable length and configured spacing, logical admission order, distinct climbing positions, occupants, and admission reservations. `Building::consumeSimulationEvents()` returns and clears value events; no event callback runs during a simulation phase. Phase-completion and traversal-lifecycle events make tick ordering observable.

Agents can be created with `Building::createAgent()` and resolved with `lookupAgent()`. Replacement interaction points, device operations, and traversal resources follow the same create/lookup/remove pattern. Each category has a distinct handle type, lookups return an explicit diagnostic on invalid or removed handles, and IDs are never reused. The raw-pointer `addAgentToSector()` overload remains only as a legacy ownership-transfer seam during migration.

The graphical application continues to call `Building::update(elapsedSeconds)`. That method accumulates render-frame time and advances only complete fixed ticks, so frame rate no longer determines simulation progress or operation completion.

## Prerequisites

- Windows x64
- Visual Studio with the MSVC `v145` toolset and Windows 10 SDK

Run commands from the repository root in a Developer Command Prompt, or invoke the full path to `MSBuild.exe`.

## Build and run only the headless scenario

```bat
msbuild build\headless.vcxproj /m /p:Configuration=Debug /p:Platform=x64
bin\x64\Debug\phosphorus-fluoride-headless.exe
```

Use `Release` in both paths to build and run the optimized configuration:

```bat
msbuild build\headless.vcxproj /m /p:Configuration=Release /p:Platform=x64
bin\x64\Release\phosphorus-fluoride-headless.exe
```

Building `headless.vcxproj` automatically builds its `core.vcxproj` project reference. A successful run prints a `PASS` line and returns exit code 0. A failed assertion prints a `FAIL` line and returns a nonzero exit code.

## Build the complete solution

The shared static library, graphical application, and headless target are all in `build\imgui.sln`:

```bat
msbuild build\imgui.sln /m /p:Configuration=Debug /p:Platform=x64
msbuild build\imgui.sln /m /p:Configuration=Release /p:Platform=x64
```

The graphical executable remains `bin\x64\<Configuration>\imgui.exe` and retains its existing runtime resource and DLL copy steps.
