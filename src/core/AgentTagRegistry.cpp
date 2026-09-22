#include "core/AgentTagRegistry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/Building.h"
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

	AgentColourProperty const* AgentTagRegistry::getAgentTagColour(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getColour();
	}

	AgentWalkSpeedModifierProperty const*
	AgentTagRegistry::getAgentTagWalkSpeedModifier(AgentTagId id) const
	{
		auto const* tag = mTags.find(id);
		if (!tag)
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		return tag->getWalkSpeedModifier();
	}

	void AgentTagRegistry::registerBuilding(Building& building)
	{
		mLoadedBuildings.insert(&building);
	}

	void AgentTagRegistry::unregisterBuilding(Building& building)
	{
		mLoadedBuildings.erase(&building);
	}

	std::vector<LoadedAgentTagUsage> AgentTagRegistry::getLoadedAgentTagUsage(
		AgentTagId id) const
	{
		if (!mTags.find(id))
			throw std::out_of_range(std::format(
				"Agent tag {} is not defined in this registry", id.value));

		std::vector<LoadedAgentTagUsage> usage;
		usage.reserve(mLoadedBuildings.size());
		for (auto const* building : mLoadedBuildings)
		{
			if (!building) continue;
			usage.push_back({ building, building->countAgentTagAssignments(id) });
		}
		return usage;
	}

	uint64_t AgentTagRegistry::getLoadedAgentTagUsageCount(AgentTagId id) const
	{
		uint64_t count{ 0 };
		for (auto const& entry : getLoadedAgentTagUsage(id)) count += entry.agentCount;
		return count;
	}

	bool AgentTagRegistry::hasLoadedBuilding(Building const* building) const
	{
		return building && mLoadedBuildings.contains(const_cast<Building*>(building));
	}

	bool AgentTagRegistry::nameIsUnique(std::string const& name, AgentTagId except) const
	{
		for (auto const& [id, tag] : mTags.entries())
		{
			if (id != except && tag->getName() == name) return false;
		}
		return true;
	}

	uint64_t AgentTagRegistry::allocatePropertyRevision()
	{
		// Zero is reserved as "no revision". As with stable entity IDs, refuse
		// exhaustion rather than wrapping and reusing a value.
		if (mNextPropertyRevision == 0
			|| mNextPropertyRevision == std::numeric_limits<uint64_t>::max())
		{
			throw std::overflow_error(
				"This registry has issued every Agent property revision");
		}
		return mNextPropertyRevision++;
	}

	bool AgentTagRegistry::colourAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));

		for (auto const* building : mLoadedBuildings)
		{
			if (!building) continue;
			for (auto const& [agentId, agent] : building->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getColour()) continue;
					return reject(std::format(
						"Cannot add Colour to Agent tag #{}: Agent '{}' in Building '{}' already inherits Colour from #{}",
						target->getName(), agent->getName(), building->getName(),
						source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::walkSpeedModifierAdditionIsValid(AgentTagId id,
		std::string* diagnostic) const
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* target = mTags.find(id);
		if (!target)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));

		for (auto const* building : mLoadedBuildings)
		{
			if (!building) continue;
			for (auto const& [agentId, agent] : building->mAgents.entries())
			{
				(void)agentId;
				if (!agent || !agent->hasAgentTag(id)) continue;
				for (auto const assigned : agent->getAgentTagIds())
				{
					if (assigned == id) continue;
					auto const* source = mTags.find(assigned);
					if (!source || !source->getWalkSpeedModifier()) continue;
					return reject(std::format(
						"Cannot add Walk speed modifier to Agent tag #{}: Agent '{}' in Building '{}' already inherits Walk speed modifier from #{}",
						target->getName(), agent->getName(), building->getName(),
						source->getName()));
				}
			}
		}
		if (diagnostic) diagnostic->clear();
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
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));

		// Judge every dependent Building before mutating any of them. Assignment
		// changes are paused-only, and a shared delete must never clear one
		// Building before discovering that another cannot participate.
		for (auto const* building : mLoadedBuildings)
		{
			if (building && building->countAgentTagAssignments(id) > 0
				&& !building->isSimulationPaused())
			{
				return reject(std::format(
					"Pause Building '{}' before deleting Agent tag #{}",
					building->getName(), tag->getName()));
			}
		}

		// Every loaded assignment goes before the definition. From the first
		// write onward no loaded Building can be left with a stale reference, and
		// no later step can refuse after the complete preflight above.
		for (auto* building : mLoadedBuildings)
			if (building) building->clearAgentTagAssignments(id);
		mTags.remove(id);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagColour(AgentTagId id, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (tag->getColour())
			return reject(std::format("Agent tag #{} already has Colour", tag->getName()));
		if (!colourAdditionIsValid(id, diagnostic)) return false;

		try
		{
			tag->setColour({ EditorDefaultAgentColour, allocatePropertyRevision() });
		}
		catch (std::exception const& error)
		{
			return reject(error.what());
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagColour(AgentTagId id, AgentColour colour,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getColour();
		if (!current)
			return reject(std::format("Agent tag #{} has no Colour", tag->getName()));
		if (current->value == colour)
			return reject("The Agent Colour is unchanged");

		try
		{
			tag->setColour({ colour, allocatePropertyRevision() });
		}
		catch (std::exception const& error)
		{
			return reject(error.what());
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagColour(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (!tag->getColour())
			return reject(std::format("Agent tag #{} has no Colour", tag->getName()));
		tag->removeColour();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::addAgentTagWalkSpeedModifier(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (tag->getWalkSpeedModifier())
			return reject(std::format(
				"Agent tag #{} already has Walk speed modifier", tag->getName()));
		if (!walkSpeedModifierAdditionIsValid(id, diagnostic)) return false;

		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setWalkSpeedModifier({ DefaultAgentWalkSpeedModifierRange, revision });
		for (auto* building : mLoadedBuildings)
			if (building) building->addAgentTagWalkSpeedModifierSamples(
				id, *tag->getWalkSpeedModifier());
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::setAgentTagWalkSpeedModifier(AgentTagId id,
		AgentModifierRange range, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		auto const* current = tag->getWalkSpeedModifier();
		if (!current)
			return reject(std::format(
				"Agent tag #{} has no Walk speed modifier", tag->getName()));
		if (!agentWalkSpeedModifierRangeIsValid(range, diagnostic)) return false;
		if (current->range == range)
			return reject("The Agent Walk speed modifier range is unchanged");
		if (getLoadedAgentTagUsageCount(id) > 0)
		{
			return reject(
				"Remove loaded Agent assignments before changing a Walk speed modifier range");
		}

		uint64_t revision{ 0 };
		try { revision = allocatePropertyRevision(); }
		catch (std::exception const& error) { return reject(error.what()); }
		tag->setWalkSpeedModifier({ range, revision });
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::removeAgentTagWalkSpeedModifier(AgentTagId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* tag = mTags.find(id);
		if (!tag)
			return reject(std::format(
				"Agent tag {} is not defined in this registry", id.value));
		if (!tag->getWalkSpeedModifier())
			return reject(std::format(
				"Agent tag #{} has no Walk speed modifier", tag->getName()));
		for (auto* building : mLoadedBuildings)
			if (building) building->clearAgentTagWalkSpeedModifierSamples(id);
		tag->removeWalkSpeedModifier();
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentTagRegistry::loadedBuildingAssignmentsAreValid(
		AgentTagRegistry const& definitions, std::string* diagnostic) const
	{
		for (auto const* building : mLoadedBuildings)
		{
			if (building
				&& !building->agentTagAssignmentsAreValid(definitions, diagnostic))
				return false;
		}
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
			auto const* colour = tag->getColour();
			auto const* walkSpeed = tag->getWalkSpeedModifier();
			if (colour || walkSpeed)
			{
				serializer.beginArray("properties");
				if (colour)
				{
					serializer.beginMap("");
					serializer.writeString("type", "colour");
					serializer.writeUint64("revision", colour->revision);
					serializer.writeUint8("r", colour->value.r);
					serializer.writeUint8("g", colour->value.g);
					serializer.writeUint8("b", colour->value.b);
					serializer.endMap();
				}
				if (walkSpeed)
				{
					serializer.beginMap("");
					serializer.writeString("type", "walkSpeedModifier");
					serializer.writeUint64("revision", walkSpeed->revision);
					serializer.writeFloat("min", walkSpeed->range.minimum);
					serializer.writeFloat("max", walkSpeed->range.maximum);
					serializer.endMap();
				}
				serializer.endArray();
			}
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
		std::set<uint64_t> propertyRevisions;
		uint64_t greatestPropertyRevision{ 0 };
		serializer.beginArray("tags");
		while (serializer.nextArrayItem())
		{
			serializer.beginMap("");
			auto const id = AgentTagId{ serializer.readUint64("id") };
			auto name = serializer.readString("name");
			auto tag = AgentTag::create(name);
			if (serializer.hasField("properties"))
			{
				bool hasColour{ false };
				bool hasWalkSpeedModifier{ false };
				serializer.beginArray("properties");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					auto const type = serializer.readString("type");
					if (type != "colour" && type != "walkSpeedModifier")
						throw SerializationException(std::format(
							"Unsupported Agent property type '{}'", type));
					if (type == "colour" && hasColour)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Colour", name));
					if (type == "walkSpeedModifier" && hasWalkSpeedModifier)
						throw SerializationException(std::format(
							"Serialized Agent tag #{} contains more than one Walk speed modifier",
							name));
					auto const revision = serializer.readUint64("revision");
					if (revision == 0)
						throw SerializationException(
							"Serialized Agent property revision cannot be zero");

					if (type == "colour")
					{
						AgentColour const colour{
							serializer.readUint8("r"), serializer.readUint8("g"),
							serializer.readUint8("b") };
						tag->setColour({ colour, revision });
						hasColour = true;
					}
					else
					{
						AgentModifierRange const range{
							serializer.readFloat("min"), serializer.readFloat("max") };
						std::string rangeDiagnostic;
						if (!agentWalkSpeedModifierRangeIsValid(range, &rangeDiagnostic))
							throw SerializationException(
								"Serialized Walk speed modifier range is invalid: "
								+ rangeDiagnostic);
						tag->setWalkSpeedModifier({ range, revision });
						hasWalkSpeedModifier = true;
					}
					serializer.endMap();
					if (!propertyRevisions.insert(revision).second)
						throw SerializationException(std::format(
							"Serialized Agent property revision {} is reused", revision));
					greatestPropertyRevision = std::max(greatestPropertyRevision, revision);
				}
				serializer.endArray();
			}
			serializer.endMap();

			if (!id) throw SerializationException("Serialized Agent tag ID cannot be zero");
			std::string diagnostic;
			if (!AgentTag::nameIsValid(name, &diagnostic))
				throw SerializationException("Serialized Agent tag name is invalid: " + diagnostic);
			if (!names.insert(name).second)
				throw SerializationException(std::format(
					"Serialized Agent tag names must be unique (#{} appears twice)", name));
			if (!tags.restore(id, std::move(tag)))
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
		if (greatestPropertyRevision >= nextPropertyRevision)
		{
			throw SerializationException(
				"Agent property revision allocator must be above every serialized property revision");
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
