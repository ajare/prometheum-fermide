# Give Agent groups stable Building-owned identity

Status: accepted

Agent groups are authored, Building-scoped classifications whose names may change without changing which Agents belong to them. The Building owns each Agent group as an `AgentGroup` with a stable `AgentGroupId`, and an Agent retains only the optional ID; this avoids rewriting every Agent on rename, gives future group properties a durable home, and lets Building operations enforce referential integrity when a group is deleted.

## Considered options

- **Copy the group name into each Agent.** Rejected because rename becomes a distributed update, identity depends on mutable text, and future group properties have no owner.
- **Give Agents pointers or shared ownership.** Rejected because non-owning pointers complicate deletion and deserialization, while shared ownership could keep a deleted group alive.

## Consequences

Group creation, rename, deletion, and Agent assignment go through Building operations. Persisted Agent assignments use Building-local IDs, while clipboard data uses names so an Agent remains portable between Buildings; pasting creates a missing group with that name. Deleting an Agent group clears every assignment to it, and runtime simulation snapshots and events remain unaware of this editor-only classification.
