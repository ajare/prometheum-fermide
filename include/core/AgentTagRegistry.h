#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/AgentTag.h"
#include "core/EntityId.h"
#include "core/EntityRegistry.h"
#include "core/Serializable.h"

namespace core
{
	class Building;

	struct LoadedAgentTagUsage
	{
		Building const* building{ nullptr };
		uint32_t agentCount{ 0 };
	};

	// A separately persisted namespace for Agent tags. IDs belong to this
	// registry, remain stable across rename, and are never reused.
	class AgentTagRegistry : public Serializable
	{
		std::string mUuid;
		EntityRegistry<AgentTagId, AgentTag> mTags;
		uint64_t mNextPropertyRevision{ 1 };
		// Buildings register while this shared registry is attached. Raw pointers
		// are safe here because Building unregisters before destruction.
		std::set<Building*> mLoadedBuildings;

		bool childrenModified() const override;
		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;
		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		explicit AgentTagRegistry(std::string uuid);
		bool nameIsUnique(std::string const& name, AgentTagId except = {}) const;
		uint64_t allocatePropertyRevision();
		bool colourAdditionIsValid(AgentTagId id, std::string* diagnostic) const;
		bool walkSpeedModifierAdditionIsValid(AgentTagId id,
			std::string* diagnostic) const;
		void registerBuilding(Building& building);
		void unregisterBuilding(Building& building);

		friend class Building;

	public:
		static std::shared_ptr<AgentTagRegistry> create();
		static std::shared_ptr<AgentTagRegistry> loadFrom(std::string const& filepath);

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

		// Live usage is derived from every loaded Building sharing this exact
		// registry instance. Closed Buildings are deliberately unknowable.
		std::vector<LoadedAgentTagUsage> getLoadedAgentTagUsage(AgentTagId id) const;
		uint64_t getLoadedAgentTagUsageCount(AgentTagId id) const;
		bool hasLoadedBuilding(Building const* building) const;

		AgentTagId addAgentTag(std::string const& name);
		bool renameAgentTag(AgentTagId id, std::string const& name,
			std::string* diagnostic = nullptr);
		bool deleteAgentTag(AgentTagId id, std::string* diagnostic = nullptr);

		// Colour is unique within a tag. Addition and assignment both preflight
		// inherited-property conflicts across every loaded dependent Building.
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

		// Used by registry undo/redo to reject a prospective definition set that
		// would reinterpret any currently loaded Agent assignment. Buildings whose
		// coordinated snapshots have already been validated may be excluded.
		bool loadedBuildingAssignmentsAreValid(AgentTagRegistry const& definitions,
			std::string* diagnostic = nullptr,
			std::vector<Building const*> const& excludedBuildings = {}) const;

		void saveTo(std::string const& filepath);
	};
}
