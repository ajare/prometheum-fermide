# Give Agent groups stable World-owned identity

Status: accepted

Agent groups are authored, World-scoped classifications whose names may change without changing which Agents belong to them. The World owns each Agent group as an `AgentGroup` with a stable `AgentGroupId`, and an Agent retains only the optional ID; this avoids rewriting every Agent on rename, gives future group properties a durable home, and lets World operations enforce referential integrity when a group is deleted.

## Considered options

- **Copy the group name into each Agent.** Rejected because rename becomes a distributed update, identity depends on mutable text, and future group properties have no owner.
- **Give Agents pointers or shared ownership.** Rejected because non-owning pointers complicate deletion and deserialization, while shared ownership could keep a deleted group alive.

## Consequences

Group creation, rename, deletion, and Agent assignment go through World operations. Persisted Agent assignments use World-local IDs, while clipboard data uses names so an Agent remains portable between Worlds; pasting creates a missing group with that name. Deleting an Agent group clears every assignment to it, and runtime simulation snapshots and events remain unaware of this editor-only classification.
