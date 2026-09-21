#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/AgentTag.h"
#include "core/EntityId.h"
#include "core/EntityRegistry.h"
#include "core/Serializable.h"

namespace core
{
	// A separately persisted namespace for Agent tags. IDs belong to this
	// registry, remain stable across rename, and are never reused.
	class AgentTagRegistry : public Serializable
	{
		std::string mUuid;
		EntityRegistry<AgentTagId, AgentTag> mTags;
		uint64_t mNextPropertyRevision{ 1 };

		bool childrenModified() const override;
		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;
		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		explicit AgentTagRegistry(std::string uuid);
		bool nameIsUnique(std::string const& name, AgentTagId except = {}) const;

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

		AgentTagId addAgentTag(std::string const& name);
		bool renameAgentTag(AgentTagId id, std::string const& name,
			std::string* diagnostic = nullptr);
		bool deleteAgentTag(AgentTagId id, std::string* diagnostic = nullptr);

		void saveTo(std::string const& filepath);
	};
}
