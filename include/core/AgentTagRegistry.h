#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/AgentTag.h"
#include "core/EntityId.h"
#include "core/EntityRegistry.h"
#include "core/Serializable.h"

namespace core
{
	class World;

	struct LoadedAgentTagUsage
	{
		World const* world{ nullptr };
		uint32_t agentCount{ 0 };
	};

	// A separately persisted namespace for Agent tags. IDs belong to this
	// registry, remain stable across rename, and are never reused.
	class AgentTagRegistry : public Serializable
	{
		std::string mUuid;
		EntityRegistry<AgentTagId, AgentTag> mTags;
		uint64_t mNextPropertyRevision{ 1 };
		// Set by deserializeImpl when a parsed document predates intrinsic tag
		// display Colours; loadFrom turns it into dirty state after the base
		// deserialize clears the modified flag. Editor snapshots always carry
		// the field, so undo/redo restores never set it.
		bool mBackfilledDisplayColour{ false };
		// The exact bytes last loaded or saved provide optimistic concurrency for
		// this independently persisted document. A save never silently overwrites
		// a different on-disk revision.
		std::optional<std::filesystem::path> mDocumentPath;
		std::string mSavedDocumentContents;
		// Worlds register while this shared registry is attached. Raw pointers
		// are safe here because World unregisters before destruction.
		std::set<World*> mLoadedWorlds;

		bool childrenModified() const override;
		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;
		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		explicit AgentTagRegistry(std::string uuid);
		bool nameIsUnique(std::string const& name, AgentTagId except = {}) const;
		uint64_t allocatePropertyRevision();
		bool colourAdditionIsValid(AgentTagId id, std::string* diagnostic) const;
		bool walkSpeedModifierAdditionIsValid(AgentTagId id,
			std::string* diagnostic) const;
		bool heightModifierAdditionIsValid(AgentTagId id,
			std::string* diagnostic) const;
		void registerWorld(World& world);
		void unregisterWorld(World& world);

		friend class World;

	public:
		static std::shared_ptr<AgentTagRegistry> create();
		static std::shared_ptr<AgentTagRegistry> loadFrom(std::string const& filepath);

		// Produces a separate namespace for World Save As. Authored tag IDs,
		// allocator high-water marks, property revisions, names, and values are
		// copied exactly; only the registry document UUID changes.
		static std::shared_ptr<AgentTagRegistry> copyWithNewUuid(
			AgentTagRegistry const& source);
		bool hasEquivalentDefinitions(AgentTagRegistry const& other) const;

		static bool uuidIsValid(std::string const& uuid);

		std::string const& getUuid() const;
		uint64_t getNextAgentTagId() const;
		uint64_t getNextPropertyRevision() const;
		uint32_t getAgentTagCount() const;
		std::vector<AgentTagId> getAgentTagIds() const;
		std::vector<AgentTagId> getAgentTagIdsAlphabetically() const;
		AgentTag const* lookupAgentTag(AgentTagId id) const;
		std::string const& getAgentTagName(AgentTagId id) const;
		AgentColourProperty const* getAgentTagColour(AgentTagId id) const;
		AgentWalkSpeedModifierProperty const* getAgentTagWalkSpeedModifier(
			AgentTagId id) const;
		AgentHeightModifierProperty const* getAgentTagHeightModifier(
			AgentTagId id) const;

		// Live usage is derived from every loaded World sharing this exact
		// registry instance. Closed Worlds are deliberately unknowable.
		std::vector<LoadedAgentTagUsage> getLoadedAgentTagUsage(AgentTagId id) const;
		uint64_t getLoadedAgentTagUsageCount(AgentTagId id) const;
		bool hasLoadedWorld(World const* world) const;
		bool hasLoadedWorlds() const;

		// Reports whether the tracked file's bytes differ from the revision loaded
		// or saved by this document. Untracked new registries report no conflict.
		bool fileHasExternalChanges(std::string const& filepath) const;

		// Internal document-manager seam. The complete replacement and every loaded
		// Agent are validated before this shared instance changes.
		bool replaceDefinitionsFrom(AgentTagRegistry&& replacement,
			std::string* diagnostic);

		// Registry definitions are shared authored state. Editing them is safe only
		// when every loaded dependent World is paused, including Worlds that
		// currently assign none of the edited tags.
		bool definitionEditsAreAllowed(std::string* diagnostic = nullptr) const;

		AgentTagId addAgentTag(std::string const& name);
		bool renameAgentTag(AgentTagId id, std::string const& name,
			std::string* diagnostic = nullptr);
		bool deleteAgentTag(AgentTagId id, std::string* diagnostic = nullptr);

		// The intrinsic display Colour used to render the tag itself (for example
		// its Selection-panel chip). Every tag always has one, assigned at random
		// from AgentTagColourPalette on creation and persisted with the tag. It is
		// separate from the optional Agent Colour property below and never
		// affects Agents.
		AgentColour getAgentTagDisplayColour(AgentTagId id) const;
		bool setAgentTagDisplayColour(AgentTagId id, AgentColour colour,
			std::string* diagnostic = nullptr);

		// Colour is unique within a tag. Addition and assignment both preflight
		// inherited-property conflicts across every loaded dependent World.
		// Revisions are registry-wide, monotonic, persisted, and consumed only by
		// an accepted addition or real value change.
		bool addAgentTagColour(AgentTagId id, std::string* diagnostic = nullptr);
		bool setAgentTagColour(AgentTagId id, AgentColour colour,
			std::string* diagnostic = nullptr);
		bool removeAgentTagColour(AgentTagId id, std::string* diagnostic = nullptr);

		bool addAgentTagWalkSpeedModifier(AgentTagId id,
			std::string* diagnostic = nullptr);
		// A real range change allocates one new revision and replaces every loaded
		// inheriting Agent's sample exactly once. Unchanged and invalid ranges leave
		// definitions, samples, revisions, and dirty state untouched.
		bool setAgentTagWalkSpeedModifier(AgentTagId id, AgentModifierRange range,
			std::string* diagnostic = nullptr);
		bool removeAgentTagWalkSpeedModifier(AgentTagId id,
			std::string* diagnostic = nullptr);

		bool addAgentTagHeightModifier(AgentTagId id,
			std::string* diagnostic = nullptr);
		bool setAgentTagHeightModifier(AgentTagId id, AgentModifierRange range,
			std::string* diagnostic = nullptr);
		bool removeAgentTagHeightModifier(AgentTagId id,
			std::string* diagnostic = nullptr);

		// Used by registry undo/redo to reject a prospective definition set that
		// would reinterpret any currently loaded Agent assignment. Worlds whose
		// coordinated snapshots have already been validated may be excluded.
		bool loadedWorldAssignmentsAreValid(AgentTagRegistry const& definitions,
			std::string* diagnostic = nullptr,
			std::vector<World const*> const& excludedWorlds = {}) const;

		void saveTo(std::string const& filepath);
	};
}
