#include "core/AgentTagRegistry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

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
		return mTags.nextId();
	}

	uint64_t AgentTagRegistry::getNextPropertyRevision() const
	{
		return mNextPropertyRevision;
	}

	uint32_t AgentTagRegistry::getAgentTagCount() const
	{
		return static_cast<uint32_t>(mTags.entries().size());
	}

	std::vector<AgentTagId> AgentTagRegistry::getAgentTagIds() const
	{
		std::vector<AgentTagId> ids;
		ids.reserve(mTags.entries().size());
		for (auto const& [id, tag] : mTags.entries())
		{
			(void)tag;
			ids.push_back(id);
		}
		return ids;
	}

	std::vector<AgentTagId> AgentTagRegistry::getAgentTagIdsAlphabetically() const
	{
		auto ids = getAgentTagIds();
		std::sort(ids.begin(), ids.end(), [this](AgentTagId left, AgentTagId right)
		{
			auto const& leftName = mTags.find(left)->getName();
			auto const& rightName = mTags.find(right)->getName();
			return leftName == rightName ? left < right : leftName < rightName;
		});
		return ids;
	}

	AgentTag const* AgentTagRegistry::lookupAgentTag(AgentTagId id) const
	{
		return mTags.find(id);
	}

	std::string const& AgentTagRegistry::getAgentTagName(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format("Agent tag {} is not defined in this registry", id.value));
		return tag->getName();
	}

	bool AgentTagRegistry::nameIsUnique(std::string const& name, AgentTagId except) const
	{
		for (auto const& [id, tag] : mTags.entries())
		{
			if (id != except && tag->getName() == name) return false;
		}
		return true;
	}

	AgentTagId AgentTagRegistry::addAgentTag(std::string const& name)
	{
		std::string diagnostic;
		if (!AgentTag::nameIsValid(name, &diagnostic))
			throw std::invalid_argument(diagnostic);
		if (!nameIsUnique(name))
			throw std::invalid_argument(std::format("The Agent tag #{} already exists", name));
		if (mTags.exhausted())
			throw std::overflow_error("This registry has issued every Agent tag ID");

		auto const id = mTags.tryAdd(AgentTag::create(name));
		if (!id) throw std::overflow_error("This registry has issued every Agent tag ID");
		modify();
		return *id;
	}

	bool AgentTagRegistry::renameAgentTag(AgentTagId id, std::string const& name,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format("Agent tag {} is not defined in this registry", id.value));
		if (!AgentTag::nameIsValid(name, diagnostic)) return false;
		if (tag->getName() == name)
			return reject("The Agent tag name is unchanged");
		if (!nameIsUnique(name, id))
			return reject(std::format("The Agent tag #{} already exists", name));

		tag->setName(name);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::deleteAgentTag(AgentTagId id, std::string* diagnostic)
	{
		if (!mTags.find(id))
		{
			if (diagnostic)
				*diagnostic = std::format("Agent tag {} is not defined in this registry", id.value);
			return false;
		}
		mTags.remove(id);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
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
		serializer.writeUint64("nextAgentTagId", mTags.nextId());
		serializer.writeUint64("nextPropertyRevision", mNextPropertyRevision);
		serializer.beginArray("tags");
		// Identity order is deliberately independent of the alphabetical order
		// used by the panel, so a rename never moves a serialized definition.
		for (auto const& [id, tag] : mTags.entries())
		{
			serializer.beginMap("");
			serializer.writeUint64("id", id.value);
			serializer.writeString("name", tag->getName());
			serializer.endMap();
		}
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
		if (nextPropertyRevision == 0)
		{
			throw SerializationException("Agent tag property revision allocator cannot be zero");
		}

		EntityRegistry<AgentTagId, AgentTag> tags;
		std::set<std::string> names;
		serializer.beginArray("tags");
		while (serializer.nextArrayItem())
		{
			serializer.beginMap("");
			auto const id = AgentTagId{ serializer.readUint64("id") };
			auto name = serializer.readString("name");
			serializer.endMap();

			if (!id) throw SerializationException("Serialized Agent tag ID cannot be zero");
			std::string diagnostic;
			if (!AgentTag::nameIsValid(name, &diagnostic))
				throw SerializationException("Serialized Agent tag name is invalid: " + diagnostic);
			if (!names.insert(name).second)
				throw SerializationException(std::format(
					"Serialized Agent tag names must be unique (#{} appears twice)", name));
			if (!tags.restore(id, AgentTag::create(std::move(name))))
				throw SerializationException(std::format(
					"Serialized Agent tag IDs must be unique ({} appears twice)", id.value));
		}
		serializer.endArray();
		serializer.endMap();

		if (!tags.restoreNextId(nextAgentTagId))
		{
			throw SerializationException(
				"Agent tag allocator must be above every serialized Agent tag ID");
		}

		// Commit only after the complete document has passed validation. This is
		// also what makes undo restoration transactional on the shared instance.
		mUuid = std::move(uuid);
		mTags = std::move(tags);
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
