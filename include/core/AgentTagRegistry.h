#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/Serializable.h"

namespace core
{
	// A separately persisted namespace for Agent tags. Ticket #128 introduces
	// the empty document and its durable identity; tag definitions follow in
	// later tickets.
	class AgentTagRegistry : public Serializable
	{
		std::string mUuid;
		uint64_t mNextAgentTagId{ 1 };
		uint64_t mNextPropertyRevision{ 1 };

		bool childrenModified() const override;
		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;
		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		explicit AgentTagRegistry(std::string uuid);

	public:
		static std::shared_ptr<AgentTagRegistry> create();
		static std::shared_ptr<AgentTagRegistry> loadFrom(std::string const& filepath);

		static bool uuidIsValid(std::string const& uuid);

		std::string const& getUuid() const;
		uint64_t getNextAgentTagId() const;
		uint64_t getNextPropertyRevision() const;

		void saveTo(std::string const& filepath);
	};
}
