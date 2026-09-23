# Store Agent tags in external registry documents

Status: accepted

Agent tag definitions are shared authored data rather than part of one World, so they live in separately persisted Agent tag registry documents beside the World files that reference them. A World stores only the registry basename and expected stable UUID: the relative reference keeps a project directory movable, while UUID verification prevents a replaced file from silently reinterpreting tag identities. This was chosen over embedding definitions in every World, which would duplicate and drift a shared vocabulary, and over absolute or unrestricted relative paths, which would make projects machine-specific or allow references to escape the project directory.

## Consequences

A World may have no registry and remains valid. Registry schema and identity are independent from World schema; the World advances to schema version 10 for its optional reference, while a new empty registry starts at registry schema version 1 with non-reused tag-ID and property-revision allocators.
