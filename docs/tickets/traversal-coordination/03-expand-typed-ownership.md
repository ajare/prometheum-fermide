# 03 — Expand typed identity and central ownership

**Priority:** P0

**Difficulty:** hard

**What to build:** Add the ownership foundation for replacement coordination objects beside the legacy implementation. The building owns newly introduced agents, operations, interaction points, and traversal resources, while relationships use validated typed handles rather than new owning-pointer cycles.

**Blocked by:** 02 — Add the deterministic simulation seam

**Status:** complete

- [x] New coordination entities are created and destroyed through building-owned registries.
- [x] Distinct entity categories use type-safe stable identifiers or handles that cannot be mixed accidentally.
- [x] Looking up a removed or invalid handle fails safely with a diagnostic rather than dereferencing stale memory.
- [x] Snapshots and events identify entities by stable IDs.
- [x] New APIs do not introduce owning shared-pointer cycles or raw owning back-pointers.
- [x] Removing an idle agent invalidates its handle and leaves the headless scenario stable.
- [x] Legacy entities may coexist during expansion, but ownership of an individual replacement entity is unambiguous.
