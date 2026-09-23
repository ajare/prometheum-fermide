#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/AgentBehaviour.h"
#include "core/EntityId.h"
#include "core/EntityRegistry.h"
#include "core/Serializable.h"

namespace core
{
	class World;
	class AgentBehaviourRuntimeAdapter;

	enum class AgentBehaviourReloadDiagnosticScope
	{
		Package,
		Module,
		Agent
	};

	enum class AgentBehaviourSchemaCompatibility
	{
		Unchanged,
		Compatible,
		Incompatible
	};

	struct AgentBehaviourSchemaFieldChange
	{
		std::string path;
		AgentBehaviourSchemaCompatibility compatibility{
			AgentBehaviourSchemaCompatibility::Unchanged };
		std::string diagnostic;
	};

	// One affected authored configuration in a side-effect-free schema preview.
	// Field paths use the same dotted Record and [] List notation as assignment
	// validation, so nested migration work is never hidden behind an aggregate.
	struct AgentBehaviourSchemaMigrationItem
	{
		World* world{ nullptr };
		std::string worldName;
		AgentId agent{};
		std::string agentName;
		AgentBehaviourId behaviour{};
		std::string behaviourName;
		uint64_t fromRevision{ 0 };
		uint64_t toRevision{ 0 };
		AgentBehaviourSchemaCompatibility compatibility{
			AgentBehaviourSchemaCompatibility::Unchanged };
		std::vector<AgentBehaviourSchemaFieldChange> fields;
	};

	struct AgentBehaviourSchemaMigrationPreview
	{
		std::vector<AgentBehaviourSchemaMigrationItem> configurations;
		bool requiresExplicitMigration{ false };
	};

	// Explicit authored replacement for one incompatible configuration. The
	// candidate schema validates this ordinary C++ value; Lua is not involved.
	struct AgentBehaviourConfigurationMigration
	{
		World* world{ nullptr };
		AgentId agent{};
		AgentBehaviourConfiguration configuration;
	};

	// One item from an explicit reload preflight. Failed reload diagnostics are
	// returned to the editor rather than written into the still-live registry,
	// so reporting a candidate can never replace the previous working status.
	struct LoadedAgentBehaviourUsage
	{
		World const* world{ nullptr };
		struct Agent
		{
			AgentId id{};
			std::string name;
		};
		std::vector<Agent> agents;
	};

	struct AgentBehaviourReloadDiagnostic
	{
		AgentBehaviourReloadDiagnosticScope scope{
			AgentBehaviourReloadDiagnosticScope::Package };
		std::string worldName;
		std::string agentName;
		AgentId agent{};
		std::string behaviourName;
		AgentBehaviourId behaviour{};
		std::string moduleName;
		std::string diagnostic;
		std::string traceback;
	};

	// A separately persisted package namespace for reusable Agent behaviour
	// definitions. IDs belong to this registry, remain stable across rename,
	// are never reused, and are allocated monotonically. The registry document
	// is the package manifest; its source module paths name Lua files managed
	// inside the package directory. Modules and factories are preflighted through
	// the runtime adapter; Agent callbacks never execute in this document model.
	class AgentBehaviourRegistry : public Serializable
	{
		std::string mUuid;
		// Package revision covers the registry-local helper-module declarations.
		// A declaration change must advance it even when no behaviour definition
		// changed, so attached Worlds can report one deterministic dependency set.
		uint64_t mPackageRevision{ 1 };
		std::map<std::string, std::unique_ptr<AgentBehaviourHelperModule>> mHelperModules;
		EntityRegistry<AgentBehaviourId, AgentBehaviour> mBehaviours;
		// Exact text admitted by the last deterministic package preflight. Live
		// instances consume this cache rather than observing un-reloaded file edits.
		std::map<std::string, std::string> mSourceCache;
		// The package directory that owns the manifest and every managed source
		// module. Known once the registry has been loaded from or saved to a
		// package; a freshly created registry has no directory until first save.
		std::optional<std::filesystem::path> mPackageDirectory;
		std::optional<std::filesystem::path> mDocumentPath;
		// The exact manifest bytes last loaded or saved provide optimistic
		// concurrency for this independently persisted document.
		std::string mSavedDocumentContents;
		// Worlds register while this shared registry is attached. Raw pointers
		// are safe here because World unregisters before destruction.
		// Registration order is retained as the deterministic tie-breaker for
		// same-named Worlds during coordinated reload preflight.
		std::vector<World*> mLoadedWorlds;

		bool childrenModified() const override;
		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;
		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		explicit AgentBehaviourRegistry(std::string uuid);
		bool nameIsUnique(std::string const& name, AgentBehaviourId except = {}) const;
		// Resolves a registry-relative source module against the package
		// directory and refuses anything that escapes it or is missing.
		void requireModuleFile(std::string const& sourceModulePath,
			std::filesystem::path const& packageDirectory) const;
		void preflightPackage(std::filesystem::path const& packageDirectory);
		void registerWorld(World& world);
		void unregisterWorld(World& world);

		friend class World;
		friend class AgentBehaviourRuntimeAdapter;

	public:
		static std::shared_ptr<AgentBehaviourRegistry> create();
		static std::shared_ptr<AgentBehaviourRegistry> loadFrom(
			std::string const& manifestFilepath);

		// RFC 4122 version-4 lowercase, matching the Agent tag registry format.
		static bool uuidIsValid(std::string const& uuid);

		std::string const& getUuid() const;
		uint64_t getPackageRevision() const;
		std::vector<std::string> getHelperModuleNames() const;
		AgentBehaviourHelperModule const* lookupHelperModule(
			std::string const& name) const;
		uint64_t getNextBehaviourId() const;
		uint32_t getBehaviourCount() const;
		std::vector<AgentBehaviourId> getBehaviourIds() const;
		std::vector<AgentBehaviourId> getBehaviourIdsAlphabetically() const;
		AgentBehaviour const* lookupAgentBehaviour(AgentBehaviourId id) const;
		std::string const& getBehaviourName(AgentBehaviourId id) const;

		bool hasLoadedWorld(World const* world) const;
		bool hasLoadedWorlds() const;
		std::vector<LoadedAgentBehaviourUsage> getLoadedAgentBehaviourUsage(
			AgentBehaviourId id) const;
		uint64_t getLoadedAgentBehaviourUsageCount(AgentBehaviourId id) const;

		// Reports whether the manifest's bytes differ from the revision loaded
		// or saved by this document. Untracked new registries report no conflict.
		bool fileHasExternalChanges(std::string const& manifestFilepath) const;

		// Save As uses an independent UUID while preserving the complete behaviour
		// namespace, revisions, schema history, helper declarations, and allocators.
		std::shared_ptr<AgentBehaviourRegistry> makeIndependentCopy() const;
		bool hasEquivalentDefinitions(AgentBehaviourRegistry const& other) const;

		// Internal document-manager seam. The complete replacement is validated
		// before this shared instance changes; UUID identity must match.
		bool previewDefinitionsFrom(AgentBehaviourRegistry& replacement,
			AgentBehaviourSchemaMigrationPreview& preview,
			std::string* diagnostic = nullptr) const;
		bool replaceDefinitionsFrom(AgentBehaviourRegistry&& replacement,
			std::string* diagnostic,
			std::vector<AgentBehaviourReloadDiagnostic>* reloadDiagnostics = nullptr,
			std::vector<AgentBehaviourConfigurationMigration> const& migrations = {});

		// Registry definitions are shared authored state. Editing them is safe
		// only when every loaded dependent World is paused.
		bool definitionEditsAreAllowed(std::string* diagnostic = nullptr) const;

		// Authored definition edits. The source module file must exist inside
		// the package directory whenever the directory is known. Revisions are
		// per-behaviour, monotonic, and persisted; a new behaviour starts at 1.
		AgentBehaviourId addAgentBehaviour(std::string const& name,
			std::string const& sourceModulePath,
			std::vector<AgentBehaviourSchemaField> schema);
		bool renameAgentBehaviour(AgentBehaviourId id, std::string const& name,
			std::string* diagnostic = nullptr);
		bool deleteAgentBehaviour(AgentBehaviourId id,
			std::string* diagnostic = nullptr);
		// Explicit coordinated deletion. Every affected World and replacement
		// runtime is preflighted before the definition or any assignment changes.
		bool deleteAgentBehaviourClearingAssignments(AgentBehaviourId id,
			std::string* diagnostic = nullptr);

		void saveTo(std::string const& manifestFilepath);
	};
}
