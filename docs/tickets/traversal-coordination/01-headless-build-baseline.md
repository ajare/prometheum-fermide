# 01 — Establish a coherent headless build baseline

**Priority:** P0

**Difficulty:** medium

**What to build:** Restore one coherent, buildable version of the existing simulation and add a headless scenario executable that can construct a minimal world, direct one agent through an ordinary path, advance time, and report the observable result. This is prefactoring only: it creates the safe feedback loop required for the redesign without introducing the new coordination model yet.

**Blocked by:** None — can start immediately

**Status:** ready-for-agent

- [x] Debug and Release application builds succeed with the supported MSVC toolchain.
- [x] Inconsistent partial interaction APIs are reconciled so every declared type and override has one valid implementation.
- [x] A headless executable builds without linking SDL, ImGui, rendering, or audio.
- [x] A smoke scenario constructs a minimal world and agent through public simulation APIs.
- [x] The scenario advances the simulation and verifies that the agent reaches an ordinary destination.
- [x] The existing graphical application still starts and renders the same baseline world.
- [x] Build and execution instructions for the headless target are documented.
