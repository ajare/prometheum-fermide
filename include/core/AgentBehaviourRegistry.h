#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/AgentBehaviour.h"
#include "core/EntityId.h"
#include "core/EntityRegistry.h"
#include "core/Serializable.h"

namespace core
{
	class Building;
	class AgentBehaviourRuntimeAdapter;

	// A separately persisted package namespace for reusable Agent behaviour
	// definitions. IDs belong to this registry, remain stable across rename,
	// are never reused, and are allocated monotonically. The registry document
	// is the package manifest; its source module paths name Lua files managed
	// inside the package directory. Modules and factories are preflighted through
	// the runtime adapter; Agent callbacks never execute in this document model.
	class AgentBehaviourRegistry : public Serializable
	{
		std::string mUuid;
		EntityRegistry<AgentBehaviourId, AgentBehaviour> mBehaviours;
		// The package directory that owns the manifest and every managed source
		// module. Known once the registry has been loaded from or saved to a
		// package; a freshly created registry has no directory until first save.
		std::optional<std::filesystem::path> mPackageDirectory;
		std::optional<std::filesystem::path> mDocumentPath;
		// The exact manifest bytes last loaded or saved provide optimistic
		// concurrency for this independently persisted document.
		std::string mSavedDocumentContents;
		// Buildings register while this shared registry is attached. Raw pointers
		// are safe here because Building unregisters before destruction.
		std::set<Building*> mLoadedBuildings;

		bool childrenModified() const override;
		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;
		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		explicit AgentBehaviourRegistry(std::string uuid);
		bool nameIsUnique(std::string const& name, AgentBehaviourId except = {}) const;
		// Resolves a registry-relative source module against the package
		// directory and refuses anything that escapes it or is missing.
		void requireModuleFile(std::string const& sourceModulePath,
			std::filesystem::path const& packageDirectory) const;
		void preflightModule(AgentBehaviour& behaviour,
			std::filesystem::path const& packageDirectory) const;
		void registerBuilding(Building& building);
		void unregisterBuilding(Building& building);

		friend class Building;
		friend class AgentBehaviourRuntimeAdapter;

	public:
		static std::shared_ptr<AgentBehaviourRegistry> create();
		static std::shared_ptr<AgentBehaviourRegistry> loadFrom(
			std::string const& manifestFilepath);

		// RFC 4122 version-4 lowercase, matching the Agent tag registry format.
		static bool uuidIsValid(std::string const& uuid);

		std::string const& getUuid() const;
		uint64_t getNextBehaviourId() const;
		uint32_t getBehaviourCount() const;
		std::vector<AgentBehaviourId> getBehaviourIds() const;
		std::vector<AgentBehaviourId> getBehaviourIdsAlphabetically() const;
		AgentBehaviour const* lookupAgentBehaviour(AgentBehaviourId id) const;
		std::string const& getBehaviourName(AgentBehaviourId id) const;

		bool hasLoadedBuilding(Building const* building) const;
		bool hasLoadedBuildings() const;

		// Reports whether the manifest's bytes differ from the revision loaded
		// or saved by this document. Untracked new registries report no conflict.
		bool fileHasExternalChanges(std::string const& manifestFilepath) const;

		// Internal document-manager seam. The complete replacement is validated
		// before this shared instance changes; UUID identity must match.
		bool replaceDefinitionsFrom(AgentBehaviourRegistry&& replacement,
			std::string* diagnostic);

		// Registry definitions are shared authored state. Editing them is safe
		// only when every loaded dependent Building is paused.
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

		void saveTo(std::string const& manifestFilepath);
	};
}
