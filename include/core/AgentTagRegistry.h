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

		// Live usage is derived from every loaded Building sharing this exact
		// registry instance. Closed Buildings are deliberately unknowable.
		std::vector<LoadedAgentTagUsage> getLoadedAgentTagUsage(AgentTagId id) const;
		uint64_t getLoadedAgentTagUsageCount(AgentTagId id) const;
		bool hasLoadedBuilding(Building const* building) const;

		AgentTagId addAgentTag(std::string const& name);
		bool renameAgentTag(AgentTagId id, std::string const& name,
			std::string* diagnostic = nullptr);
		bool deleteAgentTag(AgentTagId id, std::string* diagnostic = nullptr);

		void saveTo(std::string const& filepath);
	};
}
