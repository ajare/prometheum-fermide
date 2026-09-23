# Adopt World as the complete simulation aggregate name

Status: accepted

The complete simulation aggregate is named `World`. The term covers its spatial structure, movement network, devices, agents, backgrounds, facades, and transport systems without implying that every simulation represents one conventional architectural structure. Domain language, C++ APIs, filenames, UI text, tests, diagnostics, and current documentation use `World` consistently.

## Considered options

- **Keep an architecture-specific aggregate name.** Rejected because it describes only one common kind of simulated environment and conflicts with the broader contents and scenarios owned by the aggregate.
- **Retain compatibility aliases in the C++ API.** Rejected because parallel names would preserve ambiguity and let obsolete vocabulary continue to spread.
- **Limit the change to user-facing text.** Rejected because different names in the interface, domain model, and implementation would make navigation and communication less precise.

## Consequences

- The aggregate class and its source files are `World`, `World.h`, `World.cpp`, and `WorldSerialization.cpp`; related identifiers use `world` consistently.
- Persisted World documents use the required `.world.yaml` filename suffix, distinguishing them from other YAML documents.
- `World` is the canonical glossary term and ownership scope, including World-owned Markers and Agent groups.
- Existing YAML documents remain loadable because their schema has no aggregate-name wrapper or discriminator to migrate.
- Agent behaviour teardown reports `world_close`. No source-level aliases or legacy scripting value are retained.
