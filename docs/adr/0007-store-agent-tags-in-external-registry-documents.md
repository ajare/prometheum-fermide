# Store Agent tags in external registry documents

Status: accepted

Agent tag definitions are shared authored data rather than part of one Building, so they live in separately persisted Agent tag registry documents beside the Building files that reference them. A Building stores only the registry basename and expected stable UUID: the relative reference keeps a project directory movable, while UUID verification prevents a replaced file from silently reinterpreting tag identities. This was chosen over embedding definitions in every Building, which would duplicate and drift a shared vocabulary, and over absolute or unrestricted relative paths, which would make projects machine-specific or allow references to escape the project directory.

## Consequences

A Building may have no registry and remains valid. Registry schema and identity are independent from Building schema; the Building advances to schema version 10 for its optional reference, while a new empty registry starts at registry schema version 1 with non-reused tag-ID and property-revision allocators.
