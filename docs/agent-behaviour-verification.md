# Agent behaviour end-to-end verification

Ticket #164 closes the Agent behaviour feature with one public-facade workflow and
an explicit prerequisite coverage map. The automated workflow is
`src/headless/AgentBehaviourWorkflowSmokeChecks.cpp`; it uses the real fixed-tick
pipeline, Lua 5.4/sol2 adapter, registry package, Building persistence, document
history, and extracted ImGui panels. It does not inspect Lua stacks, command
queues, callback containers, graph internals, or traversal coordination state.

## Deterministic workflow

The shared `Shared schedule` Lua definition is assigned to three Agents with
different nested schedules, delays, fallback Markers, and failure policy. Two
fresh runs cover and compare five independent canonical digests:

- stable Agent/interaction snapshots after every attempted fixed tick;
- public semantic destination, cancellation, route-loss, activation, and
  interaction events;
- structured runtime diagnostics;
- Lua callback observations; and
- capability command observations.

The run includes named Marker destinations, one-shot timers, frozen timers while
deactivated, reactivation, ordinary pause/resume, initial route loss, safe
movement cancellation and replacement, successful and failed interactions,
instance-local failure isolation, reset, unchanged external-source reload, and
resume. Public simulation events are consumed independently after Lua observes
them. Reset and reload recreate instances from authored configuration.

The same executable also verifies current Building round trips (including the
version-11 Marker identity boundary), direct version-11 loading, deterministic
version-10 unnamed Marker migration, version-1 registry round trips, unsupported
version refusal, malformed Building/registry refusal, assignment undo/redo, and
atomic whole-document replacement.

CPU-side ImGui frames render the real registry/behaviour panel, assignment
picker, recursive schedule editor, named Marker editor, runtime status,
diagnostics/traceback/clear control, and their running-mode disabled scopes.
ImGui frame finalisation checks the panel stacks without a display server or
native dialogs.

## Parent-spec prerequisite map

The final workflow intentionally composes the public contracts below rather than
duplicating every adversarial fixture. These closed prerequisite checks run in
the same `headless-smoke` executable:

| Parent behaviour | Prerequisite check |
|---|---|
| Stable named Markers, migration, deletion refusal | #146 (`MarkerIdentitySmokeChecks`, `AgentBehaviourAssignmentSmokeChecks`) |
| Safe movement, cancellation, route loss | #147 and #152 (`MovementCommandSmokeChecks`, `AgentBehaviourRuntimeSmokeChecks`) |
| Registry package identity, schema v1, malformed package refusal | #148 (`AgentBehaviourRegistrySmokeChecks`) |
| Typed scalar/composite configuration, nested diagnostics, undo/redo | #149 and #154 (`AgentBehaviourAssignmentSmokeChecks`) |
| Real Lua/sol2 contract and protected preflight | #150 (`AgentBehaviourRuntimeSmokeChecks`) |
| Private per-Agent instances and callback/command ordering | #151 and #156 (`AgentBehaviourRuntimeSmokeChecks`) |
| Timers, semantic state, events, interactions, teardown | #153 and #155 (`AgentBehaviourRuntimeSmokeChecks`) |
| Atomic hot reload and aggregated diagnostics | #157 (`AgentBehaviourRegistrySmokeChecks`) |
| Sandbox, allocator/instruction limits | #158 (`AgentBehaviourRuntimeSmokeChecks`) |
| Storm bounds and failure scope | #159 (`AgentBehaviourRuntimeSmokeChecks`) |
| Schema reconciliation and coordinated document edits | #160 (`AgentBehaviourSchemaReconciliationSmokeChecks`) |
| Missing dependency recovery and destructive registry changes | #161 (`AgentBehaviourRegistrySmokeChecks`) |
| Used-behaviour confirmation/deletion/undo | #162 (`AgentBehaviourDeleteSmokeChecks`) |
| Clipboard portability and Save As package copying | #163 (`AgentBehaviourPortabilitySmokeChecks`) |

## Portable build contract

Both `imgui` and `prometheum-fermide-headless` link the same
`prometheum-fermide-core`; Lua and sol2 are private dependencies of that core.
CMake builds Lua 5.4.9 from the archive with SHA-256
`2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6` and
sol2 v3.5.0 from commit `e24392e9718f0616cfbba86005622a419d2f0c5d` on both
Linux/GCC and Windows/MSVC. Platform-specific system Lua or sol2 packages are
not searched.

Ordinary validation:

```sh
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

High-analysis validation:

```sh
cmake -S . -B build-high-analysis \
  -DCMAKE_BUILD_TYPE=Release -DPF_BUILD_GUI=ON -DPF_HIGH_ANALYSIS=ON
cmake --build build-high-analysis --parallel
ctest --test-dir build-high-analysis --output-on-failure
```

Use the equivalent Visual Studio x64 generator and `--config Release` on
Windows. The same CTest names and dependency pins apply.

## Manual GUI release checklist

Perform this pass with a disposable saved Building; keep simulation paused for
authored edits. It is deliberately a manual presentation/usability check, not a
second simulation implementation.

1. Create or attach an adjacent `*.behaviours` package. Externally edit its Lua
   source, choose **Reload registry**, and confirm the status/revision changes.
   Introduce a syntax error once and confirm the candidate diagnostic and
   traceback appear without replacing the working package; repair and reload.
2. Assign one schedule behaviour to two Agents. Add, remove, reorder, and edit
   nested schedule entries. Confirm duration fields and named Marker pickers are
   shown and no raw IDs or YAML are exposed.
3. Select a Marker, rename it, and confirm every schedule still resolves the
   renamed Marker. Attempt to delete it while referenced and confirm the refusal
   names every dependent Agent and nested field.
4. Start simulation. Confirm assignment/configuration/reload controls and manual
   path controls are disabled while appropriate. Pause and confirm authored
   controls return; remove/disable the behaviour and confirm manual movement
   returns.
5. Trigger one behaviour callback failure. Confirm simulation pauses, the Agent
   status changes, the structured diagnostic identifies Building/behaviour/
   Agent/callback/tick, traceback expansion works, and **Clear** removes the
   displayed diagnostic without changing authored data.
6. Exercise used-behaviour deletion and used registry detach/switch. Read the
   complete consequence text, cancel once with no changes, then confirm once and
   verify assignments clear and manual controls return.
7. Copy and paste an assigned Agent within the Building and into a compatible
   Building. Confirm schedules/Marker names survive. Try an incompatible
   registry or missing Marker and confirm the complete refusal; no Marker or
   stripped Agent is created.
8. Use **Save As** to a fresh directory. Confirm the Building, manifest, behaviour
   source, and helper modules exist beside one another, then open the copied
   Building and run it. Repeat toward an occupied package destination and confirm
   no file is clobbered.

Native file dialogs are used only in this manual pass. All automated CTest
coverage is headless and cannot display or wait on a dialog.
