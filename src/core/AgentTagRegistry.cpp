#include "core/AgentTagRegistry.h"

#include <array>
#include <cctype>
#include <format>
#include <random>

#include "core/SerializationException.h"
#include "core/YamlSerializer.h"

namespace core
{
	namespace
	{
		std::string generateUuid()
		{
			std::random_device source;
			std::array<uint8_t, 16> bytes{};
			for (auto& byte : bytes) byte = static_cast<uint8_t>(source());

			// RFC 4122 variant, version 4. The UUID is document identity rather
			// than an allocator value and remains unchanged for the file's life.
			bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);
			bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
			return std::format(
				"{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
				bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
				bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
		}
	}

	AgentTagRegistry::AgentTagRegistry(std::string uuid)
		: mUuid(std::move(uuid))
	{
	}

	std::shared_ptr<AgentTagRegistry> AgentTagRegistry::create()
	{
		return std::shared_ptr<AgentTagRegistry>(new AgentTagRegistry(generateUuid()));
	}

	std::shared_ptr<AgentTagRegistry> AgentTagRegistry::loadFrom(std::string const& filepath)
	{
		auto registry = std::shared_ptr<AgentTagRegistry>(new AgentTagRegistry(""));
		auto serializer = YamlSerializer::fromFile(filepath);
		serializer->deserialize();
		SerializationWorkData workData;
		if (!registry->deserialize(*serializer, workData))
		{
			throw SerializationException("Could not deserialize Agent tag registry");
		}
		return registry;
	}

	bool AgentTagRegistry::uuidIsValid(std::string const& uuid)
	{
		if (uuid.size() != 36 || uuid[8] != '-' || uuid[13] != '-'
			|| uuid[18] != '-' || uuid[23] != '-') return false;
		for (size_t index = 0; index < uuid.size(); ++index)
		{
			if (index == 8 || index == 13 || index == 18 || index == 23) continue;
			auto const character = static_cast<unsigned char>(uuid[index]);
			if (!std::isxdigit(character) || std::isupper(character)) return false;
		}
		return uuid[14] == '4' && (uuid[19] == '8' || uuid[19] == '9'
			|| uuid[19] == 'a' || uuid[19] == 'b');
	}

	std::string const& AgentTagRegistry::getUuid() const
	{
		return mUuid;
	}

	uint64_t AgentTagRegistry::getNextAgentTagId() const
	{
		return mNextAgentTagId;
	}

	uint64_t AgentTagRegistry::getNextPropertyRevision() const
	{
		return mNextPropertyRevision;
	}

	bool AgentTagRegistry::childrenModified() const
	{
		return false;
	}

	void AgentTagRegistry::serializeImpl(Serializer& serializer, SerializationWorkData&) const
	{
		if (!uuidIsValid(mUuid))
		{
			throw SerializationException("Cannot serialize an Agent tag registry with an invalid UUID");
		}
		serializer.beginMap("agentTagRegistry");
		serializer.writeUint32("version", 1);
		serializer.writeString("uuid", mUuid);
		serializer.writeUint64("nextAgentTagId", mNextAgentTagId);
		serializer.writeUint64("nextPropertyRevision", mNextPropertyRevision);
		serializer.beginArray("tags");
		serializer.endArray();
		serializer.endMap();
	}

	bool AgentTagRegistry::deserializeImpl(Serializer& serializer, SerializationWorkData&)
	{
		serializer.beginMap("agentTagRegistry");
		auto const version = serializer.readUint32("version");
		if (version != 1)
		{
			throw SerializationException("Unsupported Agent tag registry serialization version");
		}
		auto uuid = serializer.readString("uuid");
		if (!uuidIsValid(uuid))
		{
			throw SerializationException("Agent tag registry UUID is invalid");
		}
		auto const nextAgentTagId = serializer.readUint64("nextAgentTagId");
		auto const nextPropertyRevision = serializer.readUint64("nextPropertyRevision");
		if (nextAgentTagId == 0 || nextPropertyRevision == 0)
		{
			throw SerializationException(
				"An empty Agent tag registry must have non-zero ID allocators");
		}

		serializer.beginArray("tags");
		if (serializer.nextArrayItem())
		{
			throw SerializationException(
				"This build only supports empty Agent tag registries");
		}
		serializer.endArray();
		serializer.endMap();

		mUuid = std::move(uuid);
		mNextAgentTagId = nextAgentTagId;
		mNextPropertyRevision = nextPropertyRevision;
		return true;
	}

	void AgentTagRegistry::saveTo(std::string const& filepath)
	{
		auto serializer = YamlSerializer::toFile(filepath);
		SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		serialize(*serializer, workData);
		serializer->serialize();
		markUnmodified();
	}
}
