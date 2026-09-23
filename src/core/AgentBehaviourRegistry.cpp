#include "core/AgentBehaviourRegistry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include <variant>

#include "core/Building.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"

namespace core
{
	using namespace std;

	namespace
	{
		std::filesystem::path normalizedDocumentPath(
			std::filesystem::path const& filepath)
		{
			std::error_code error;
			auto normalized = std::filesystem::weakly_canonical(filepath, error);
			if (!error) return normalized;
			normalized = std::filesystem::absolute(filepath, error);
			return (error ? filepath : normalized).lexically_normal();
		}

		std::string readDocument(std::filesystem::path const& filepath)
		{
			std::ifstream input(filepath, std::ios::binary);
			if (!input)
				throw SerializationException(std::format(
					"Could not read Agent behaviour registry manifest {}",
					filepath.string()));
			return { std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>() };
		}

		std::string generateUuid()
		{
			std::random_device source;
			std::array<uint8_t, 16> bytes{};
			for (auto& byte : bytes) byte = static_cast<uint8_t>(source());

			// RFC 4122 variant, version 4. The UUID is document identity rather
			// than an allocator value and remains unchanged for the package's life.
			bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);
			bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
			return std::format(
				"{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
				bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
				bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
		}

		void serializeConfigurationValue(Serializer& serializer,
			AgentBehaviourConfigurationValue const& value)
		{
			visit([&](auto const& typed)
			{
				using T = decay_t<decltype(typed)>;
				if constexpr (is_same_v<T, bool>) serializer.writeBool("default", typed);
				else if constexpr (is_same_v<T, int64_t>) serializer.writeInt64("default", typed);
				else if constexpr (is_same_v<T, double>) serializer.writeDouble("default", typed);
				else if constexpr (is_same_v<T, string>) serializer.writeString("default", typed);
				else if constexpr (is_same_v<T, AgentBehaviourDuration>)
					serializer.writeUint64("default", typed.ticks);
				else serializer.writeUint64("default", typed.value);
			}, value);
		}

		AgentBehaviourConfigurationValue deserializeConfigurationValue(
			Serializer& serializer, AgentBehaviourSchemaType type)
		{
			switch (type)
			{
			case AgentBehaviourSchemaType::Boolean: return serializer.readBool("default");
			case AgentBehaviourSchemaType::Integer: return serializer.readInt64("default");
			case AgentBehaviourSchemaType::Number: return serializer.readDouble("default");
			case AgentBehaviourSchemaType::String: return serializer.readString("default");
			case AgentBehaviourSchemaType::Duration:
				return AgentBehaviourDuration{ serializer.readUint64("default") };
			case AgentBehaviourSchemaType::Marker:
				return MarkerId{ serializer.readUint64("default") };
			default:
				throw SerializationException(
					"List and Record schema fields cannot declare scalar defaults");
			}
		}

		void serializeSchemaField(Serializer& serializer,
			AgentBehaviourSchemaField const& field)
		{
			serializer.beginMap("");
			serializer.writeString("name", field.name);
			serializer.writeString("type", agentBehaviourSchemaTypeName(field.type));
			if (!field.required) serializer.writeBool("required", false);
			if (field.defaultValue) serializeConfigurationValue(serializer, *field.defaultValue);
			if (!field.children.empty())
			{
				serializer.beginArray("children");
				for (auto const& child : field.children) serializeSchemaField(serializer, child);
				serializer.endArray();
			}
			serializer.endMap();
		}

		AgentBehaviourSchemaField deserializeSchemaField(Serializer& serializer, size_t depth = 1)
		{
			if (depth > 16) throw SerializationException("Agent behaviour schema nesting exceeds 16 levels");
			serializer.beginMap("");
			AgentBehaviourSchemaField field;
			field.name = serializer.readString("name");
			auto const typeName = serializer.readString("type");
			if (!agentBehaviourSchemaTypeFromName(typeName, field.type))
			{
				throw SerializationException(std::format(
					"Unsupported Agent behaviour schema type '{}'", typeName));
			}
			field.required = serializer.readBool("required", true, true);
			if (serializer.hasField("default"))
				field.defaultValue = deserializeConfigurationValue(serializer, field.type);
			if (serializer.hasField("children"))
			{
				serializer.beginArray("children");
				while (serializer.nextArrayItem())
					field.children.push_back(deserializeSchemaField(serializer, depth + 1));
				serializer.endArray();
			}
			serializer.endMap();
			return field;
		}
	}

	AgentBehaviourRegistry::AgentBehaviourRegistry(std::string uuid)
		: mUuid(std::move(uuid))
	{
	}

	std::shared_ptr<AgentBehaviourRegistry> AgentBehaviourRegistry::create()
	{
		return std::shared_ptr<AgentBehaviourRegistry>(new AgentBehaviourRegistry(generateUuid()));
	}

	std::shared_ptr<AgentBehaviourRegistry> AgentBehaviourRegistry::loadFrom(
		std::string const& manifestFilepath)
	{
		auto const path = normalizedDocumentPath(manifestFilepath);
		auto const packageDirectory = path.parent_path();
		auto contents = readDocument(path);
		auto registry = std::shared_ptr<AgentBehaviourRegistry>(new AgentBehaviourRegistry(""));
		auto serializer = YamlSerializer::fromString(contents);
		serializer->deserialize();
		SerializationWorkData workData;
		if (!registry->deserialize(*serializer, workData))
		{
			throw SerializationException("Could not deserialize Agent behaviour registry manifest");
		}
		// Refuse a manifest whose managed source modules are missing or have
		// escaped the package. Paths were already validated lexically during
		// deserialization; here they must also exist beside the manifest.
		registry->mPackageDirectory = packageDirectory;
		for (auto const& [id, behaviour] : registry->mBehaviours.entries())
		{
			(void)id;
			registry->requireModuleFile(behaviour->getSourceModulePath(), packageDirectory);
		}
		registry->mDocumentPath = path;
		registry->mSavedDocumentContents = std::move(contents);
		return registry;
	}

	bool AgentBehaviourRegistry::uuidIsValid(std::string const& uuid)
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

	std::string const& AgentBehaviourRegistry::getUuid() const
	{
		return mUuid;
	}

	uint64_t AgentBehaviourRegistry::getNextBehaviourId() const
	{
		return mBehaviours.nextId();
	}

	uint32_t AgentBehaviourRegistry::getBehaviourCount() const
	{
		return static_cast<uint32_t>(mBehaviours.entries().size());
	}

	std::vector<AgentBehaviourId> AgentBehaviourRegistry::getBehaviourIds() const
	{
		std::vector<AgentBehaviourId> ids;
		ids.reserve(mBehaviours.entries().size());
		for (auto const& [id, behaviour] : mBehaviours.entries())
		{
			(void)behaviour;
			ids.push_back(id);
		}
		return ids;
	}

	std::vector<AgentBehaviourId> AgentBehaviourRegistry::getBehaviourIdsAlphabetically() const
	{
		auto ids = getBehaviourIds();
		std::sort(ids.begin(), ids.end(), [this](AgentBehaviourId left, AgentBehaviourId right)
		{
			auto const& leftName = mBehaviours.find(left)->getName();
			auto const& rightName = mBehaviours.find(right)->getName();
			return leftName == rightName ? left < right : leftName < rightName;
		});
		return ids;
	}

	AgentBehaviour const* AgentBehaviourRegistry::lookupAgentBehaviour(AgentBehaviourId id) const
	{
		return mBehaviours.find(id);
	}

	std::string const& AgentBehaviourRegistry::getBehaviourName(AgentBehaviourId id) const
	{
		auto const* behaviour = mBehaviours.find(id);
		if (!behaviour)
			throw std::out_of_range(std::format(
				"Agent behaviour {} is not defined in this registry", id.value));
		return behaviour->getName();
	}

	void AgentBehaviourRegistry::registerBuilding(Building& building)
	{
		mLoadedBuildings.insert(&building);
	}

	void AgentBehaviourRegistry::unregisterBuilding(Building& building)
	{
		mLoadedBuildings.erase(&building);
	}

	bool AgentBehaviourRegistry::hasLoadedBuilding(Building const* building) const
	{
		return building && mLoadedBuildings.contains(const_cast<Building*>(building));
	}

	bool AgentBehaviourRegistry::hasLoadedBuildings() const
	{
		return !mLoadedBuildings.empty();
	}

	bool AgentBehaviourRegistry::fileHasExternalChanges(
		std::string const& manifestFilepath) const
	{
		if (!mDocumentPath) return false;
		auto const path = normalizedDocumentPath(manifestFilepath);
		if (path != *mDocumentPath)
		{
			throw SerializationException(std::format(
				"Agent behaviour registry manifest is loaded from {}, not {}",
				mDocumentPath->string(), path.string()));
		}
		try
		{
			return readDocument(path) != mSavedDocumentContents;
		}
		catch (SerializationException const&)
		{
			return true;
		}
	}

	bool AgentBehaviourRegistry::replaceDefinitionsFrom(AgentBehaviourRegistry&& replacement,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (replacement.mUuid != mUuid)
		{
			return reject(std::format(
				"Agent behaviour registry UUID mismatch: loaded {}, file contains {}",
				mUuid, replacement.mUuid));
		}
		if (!definitionEditsAreAllowed(diagnostic)) return false;

		if (mBehaviours.nextId() == 0 ? replacement.mBehaviours.nextId() != 0
			: replacement.mBehaviours.nextId() != 0
				&& replacement.mBehaviours.nextId() < mBehaviours.nextId())
			return reject("Agent behaviour allocator cannot move backwards on reload");
		for (auto const& [id, candidate] : replacement.mBehaviours.entries())
		{
			auto const* previous = mBehaviours.find(id);
			if (!previous && (mBehaviours.nextId() == 0 || id.value < mBehaviours.nextId()))
				return reject("Agent behaviour reload cannot reuse a deleted ID");
			if (previous && (candidate->getRevision() < previous->getRevision()
				|| ((candidate->getSchema() != previous->getSchema()
					|| candidate->getSourceModulePath() != previous->getSourceModulePath())
					&& candidate->getRevision() == previous->getRevision())))
				return reject("Changed behaviour definitions require an increasing revision");
		}

		for (auto const* building : mLoadedBuildings)
		{
			if (building && !building->inspectAgentBehaviourAssignments(replacement,
				diagnostic)) return false;
		}

		mBehaviours = std::move(replacement.mBehaviours);
		mPackageDirectory = std::move(replacement.mPackageDirectory);
		mDocumentPath = std::move(replacement.mDocumentPath);
		mSavedDocumentContents = std::move(replacement.mSavedDocumentContents);
		markUnmodified();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentBehaviourRegistry::definitionEditsAreAllowed(std::string* diagnostic) const
	{
		for (auto const* building : mLoadedBuildings)
		{
			if (building && !building->isSimulationPaused())
			{
				if (diagnostic)
				{
					*diagnostic = std::format(
						"Pause Building '{}' before editing this Agent behaviour registry",
						building->getName());
				}
				return false;
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentBehaviourRegistry::nameIsUnique(std::string const& name,
		AgentBehaviourId except) const
	{
		for (auto const& [id, behaviour] : mBehaviours.entries())
		{
			if (id != except && behaviour->getName() == name) return false;
		}
		return true;
	}

	void AgentBehaviourRegistry::requireModuleFile(std::string const& sourceModulePath,
		std::filesystem::path const& packageDirectory) const
	{
		std::error_code error;
		auto const modulePath = std::filesystem::weakly_canonical(
			packageDirectory / sourceModulePath, error);
		if (error || !std::filesystem::is_regular_file(modulePath, error) || error)
		{
			throw SerializationException(std::format(
				"Agent behaviour source module is missing from the registry package: {}",
				sourceModulePath));
		}
		auto const relative = modulePath.lexically_relative(packageDirectory);
		if (relative.empty() || relative.is_absolute()
			|| *relative.begin() == "..")
		{
			throw SerializationException(std::format(
				"Agent behaviour source module escapes the registry package: {}",
				sourceModulePath));
		}
	}

	AgentBehaviourId AgentBehaviourRegistry::addAgentBehaviour(std::string const& rawName,
		std::string const& sourceModulePath,
		std::vector<AgentBehaviourSchemaField> schema)
	{
		auto const name = AgentBehaviour::trimName(rawName);
		std::string diagnostic;
		if (!AgentBehaviour::nameIsValid(name, &diagnostic))
			throw std::invalid_argument(diagnostic);
		if (!nameIsUnique(name))
			throw std::invalid_argument(std::format(
				"The Agent behaviour #{} already exists", name));
		if (!AgentBehaviour::sourceModulePathIsValid(sourceModulePath, &diagnostic))
			throw std::invalid_argument(diagnostic);
		if (!agentBehaviourSchemaFieldsAreValid(schema, &diagnostic))
			throw std::invalid_argument(diagnostic);
		if (!definitionEditsAreAllowed(&diagnostic))
			throw std::invalid_argument(diagnostic);
		if (mPackageDirectory) requireModuleFile(sourceModulePath, *mPackageDirectory);
		if (mBehaviours.exhausted())
			throw std::overflow_error("This registry has issued every Agent behaviour ID");

		auto const id = mBehaviours.tryAdd(AgentBehaviour::create(
			name, sourceModulePath, std::move(schema)));
		if (!id) throw std::overflow_error("This registry has issued every Agent behaviour ID");
		modify();
		return *id;
	}

	bool AgentBehaviourRegistry::renameAgentBehaviour(AgentBehaviourId id,
		std::string const& rawName, std::string* diagnostic)
	{
		auto const name = AgentBehaviour::trimName(rawName);
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto* behaviour = mBehaviours.find(id);
		if (!behaviour)
			return reject(std::format(
				"Agent behaviour {} is not defined in this registry", id.value));
		if (!AgentBehaviour::nameIsValid(name, diagnostic)) return false;
		if (behaviour->getName() == name)
			return reject("The Agent behaviour name is unchanged");
		if (!nameIsUnique(name, id))
			return reject(std::format("The Agent behaviour #{} already exists", name));
		if (!definitionEditsAreAllowed(diagnostic)) return false;

		behaviour->setName(name);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentBehaviourRegistry::deleteAgentBehaviour(AgentBehaviourId id,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		auto const* behaviour = mBehaviours.find(id);
		if (!behaviour)
			return reject(std::format(
				"Agent behaviour {} is not defined in this registry", id.value));
		if (!definitionEditsAreAllowed(diagnostic)) return false;
		for (auto const* building : mLoadedBuildings)
		{
			if (!building) continue;
			for (auto const& [agentId, agent] : building->mAgents.entries())
			{
				if (agent && agent->getBehaviourAssignment()
					&& agent->getBehaviourAssignment()->behaviour == id)
					return reject(format("Agent behaviour '{}' is assigned to Agent '{}' ({}) in Building '{}'",
						behaviour->getName(), agent->getName(), agentId.value,
						building->getName()));
			}
		}
		mBehaviours.remove(id);
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentBehaviourRegistry::childrenModified() const
	{
		return false;
	}

	void AgentBehaviourRegistry::serializeImpl(Serializer& serializer,
		SerializationWorkData&) const
	{
		if (!uuidIsValid(mUuid))
		{
			throw SerializationException(
				"Cannot serialize an Agent behaviour registry with an invalid UUID");
		}
		serializer.beginMap("agentBehaviourRegistry");
		serializer.writeUint32("version", 1);
		serializer.writeString("uuid", mUuid);
		serializer.writeUint64("nextBehaviourId", mBehaviours.nextId());
		serializer.beginArray("behaviours");
		// Identity order is deliberately independent of the alphabetical order
		// used by panels, so a rename never moves a serialized definition.
		for (auto const& [id, behaviour] : mBehaviours.entries())
		{
			serializer.beginMap("");
			serializer.writeUint64("id", id.value);
			serializer.writeString("name", behaviour->getName());
			serializer.writeUint64("revision", behaviour->getRevision());
			serializer.writeString("source", behaviour->getSourceModulePath());
			if (!behaviour->getSchema().empty())
			{
				serializer.beginArray("schema");
				for (auto const& field : behaviour->getSchema())
					serializeSchemaField(serializer, field);
				serializer.endArray();
			}
			serializer.endMap();
		}
		serializer.endArray();
		serializer.endMap();
	}

	bool AgentBehaviourRegistry::deserializeImpl(Serializer& serializer,
		SerializationWorkData&)
	{
		serializer.beginMap("agentBehaviourRegistry");
		auto const version = serializer.readUint32("version");
		if (version != 1)
		{
			throw SerializationException(
				"Unsupported Agent behaviour registry serialization version");
		}
		auto uuid = serializer.readString("uuid");
		if (!uuidIsValid(uuid))
		{
			throw SerializationException("Agent behaviour registry UUID is invalid");
		}
		auto const nextBehaviourId = serializer.readUint64("nextBehaviourId");

		EntityRegistry<AgentBehaviourId, AgentBehaviour> behaviours;
		std::set<std::string> names;
		serializer.beginArray("behaviours");
		while (serializer.nextArrayItem())
		{
			serializer.beginMap("");
			auto const id = AgentBehaviourId{ serializer.readUint64("id") };
			auto name = serializer.readString("name");
			auto const revision = serializer.readUint64("revision");
			auto source = serializer.readString("source");
			std::vector<AgentBehaviourSchemaField> schema;
			if (serializer.hasField("schema"))
			{
				serializer.beginArray("schema");
				while (serializer.nextArrayItem())
					schema.push_back(deserializeSchemaField(serializer));
				serializer.endArray();
			}
			serializer.endMap();

			if (!id) throw SerializationException("Serialized Agent behaviour ID cannot be zero");
			if (revision == 0)
				throw SerializationException("Serialized Agent behaviour revision cannot be zero");
			std::string diagnostic;
			if (!AgentBehaviour::nameIsValid(name, &diagnostic))
				throw SerializationException("Serialized Agent behaviour name is invalid: "
					+ diagnostic);
			if (!names.insert(name).second)
				throw SerializationException(std::format(
					"Serialized Agent behaviour names must be unique (#{} appears twice)",
					name));
			if (!AgentBehaviour::sourceModulePathIsValid(source, &diagnostic))
				throw SerializationException(
					"Serialized Agent behaviour source module path is invalid: "
					+ diagnostic);
			if (!agentBehaviourSchemaFieldsAreValid(schema, &diagnostic))
				throw SerializationException("Serialized Agent behaviour schema is invalid: "
					+ diagnostic);
			if (!behaviours.restore(id, AgentBehaviour::create(std::move(name),
				std::move(source), std::move(schema), revision)))
			{
				throw SerializationException(std::format(
					"Serialized Agent behaviour IDs must be unique ({} appears twice)",
					id.value));
			}
		}
		serializer.endArray();
		serializer.endMap();

		if (!behaviours.restoreNextId(nextBehaviourId))
		{
			throw SerializationException(
				"Agent behaviour allocator must be above every serialized Agent behaviour ID");
		}

		// Commit only after the complete manifest has passed validation, making
		// load and any future snapshot restore transactional.
		mUuid = std::move(uuid);
		mBehaviours = std::move(behaviours);
		return true;
	}

	void AgentBehaviourRegistry::saveTo(std::string const& manifestFilepath)
	{
		auto const path = normalizedDocumentPath(manifestFilepath);
		if (mDocumentPath)
		{
			if (path != *mDocumentPath)
			{
				throw SerializationException(std::format(
					"Agent behaviour registry manifest is loaded from {}, not {}",
					mDocumentPath->string(), path.string()));
			}
			if (fileHasExternalChanges(path.string()))
			{
				throw SerializationException(std::format(
					"Agent behaviour registry manifest {} changed outside the editor; reload it before saving",
					path.string()));
			}
		}

		for (auto const& [id, behaviour] : mBehaviours.entries())
		{
			(void)id;
			requireModuleFile(behaviour->getSourceModulePath(), path.parent_path());
		}
		auto serializer = YamlSerializer::toFile(path.string());
		SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		serialize(*serializer, workData);
		serializer->serialize();
		// Record exactly what reached disk. If this read is refused, retain
		// dirty state rather than claiming a revision that cannot be checked.
		auto contents = readDocument(path);
		mPackageDirectory = path.parent_path();
		mDocumentPath = path;
		mSavedDocumentContents = std::move(contents);
		markUnmodified();
	}
}
