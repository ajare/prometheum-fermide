# Headless simulation smoke scenario

The `headless` target builds the simulation core and a deterministic smoke scenario without SDL, ImGui, OpenGL, rendering, or audio dependencies. The scenario creates one corridor through the public `Building` API, routes one agent between two ordinary marker vertices, advances the building in fixed ticks, and exits unsuccessfully if the destination is not reached or two identical runs produce different snapshots or events.

## Deterministic simulation API

`Building::advanceTick()` and `Building::advanceTicks()` are the headless seam. Each tick is `Building::getFixedTimestep()` (1/60 second) and runs these phases in order:

1. resource advancement;
2. intent collection;
3. allocation;
4. movement;
5. commit;
6. cleanup and event publication.

The intent, allocation, and commit phases are explicit migration seams; legacy movement still performs those parts synchronously during movement until the replacement traversal protocol is introduced. Resource updates and stable-ID-ordered agent updates are already separated.

`Building::getSimulationSnapshot()` returns a value snapshot containing the tick and stable agent IDs, sectors, positions, path state, and path progress. It also exposes the building-owned interaction points, device operations, and traversal resources by stable typed ID. `Building::consumeSimulationEvents()` returns and clears value events; no event callback runs during a simulation phase. Phase-completion events make tick ordering observable.

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

A successful run prints a `PASS` line and returns exit code 0. A failed assertion prints a `FAIL` line and returns a nonzero exit code.

## Build the complete solution

The existing graphical application and headless target are both in `build\imgui.sln`:

```bat
msbuild build\imgui.sln /m /p:Configuration=Debug /p:Platform=x64
msbuild build\imgui.sln /m /p:Configuration=Release /p:Platform=x64
```

The graphical executable remains `bin\x64\<Configuration>\imgui.exe` and retains its existing runtime resource and DLL copy steps.
