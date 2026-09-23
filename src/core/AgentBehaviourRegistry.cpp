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

#include "core/AgentBehaviourRuntime.h"
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
			AgentBehaviourConfigurationValue const& value, string const& name)
		{
			visit([&](auto const& typed)
			{
				using T = decay_t<decltype(typed)>;
				if constexpr (is_same_v<T, bool>) serializer.writeBool(name, typed);
				else if constexpr (is_same_v<T, int64_t>) serializer.writeInt64(name, typed);
				else if constexpr (is_same_v<T, double>) serializer.writeDouble(name, typed);
				else if constexpr (is_same_v<T, string>) serializer.writeString(name, typed);
				else if constexpr (is_same_v<T, AgentBehaviourDuration>)
					serializer.writeUint64(name, typed.ticks);
				else if constexpr (is_same_v<T, MarkerId>)
					serializer.writeUint64(name, typed.value);
				else if constexpr (is_same_v<T, AgentBehaviourConfigurationList>)
				{
					serializer.beginArray(name);
					for (auto const& item : typed)
					{
						serializer.beginMap("");
						serializer.writeString("type",
							agentBehaviourConfigurationValueTypeName(item));
						serializeConfigurationValue(serializer, item, "value");
						serializer.endMap();
					}
					serializer.endArray();
				}
				else
				{
					serializer.beginArray(name);
					for (auto const& [field, item] : typed)
					{
						serializer.beginMap("");
						serializer.writeString("field", field);
						serializer.writeString("type",
							agentBehaviourConfigurationValueTypeName(item));
						serializeConfigurationValue(serializer, item, "value");
						serializer.endMap();
					}
					serializer.endArray();
				}
			}, value.value);
		}

		AgentBehaviourConfigurationValue deserializeConfigurationValue(
			Serializer& serializer, AgentBehaviourSchemaField const& field,
			string const& name, size_t depth = 1)
		{
			if (depth > MaxAgentBehaviourConfigurationDepth)
				throw SerializationException(
					"Agent behaviour default configuration nesting exceeds 16 levels");
			switch (field.type)
			{
			case AgentBehaviourSchemaType::Boolean: return serializer.readBool(name);
			case AgentBehaviourSchemaType::Integer: return serializer.readInt64(name);
			case AgentBehaviourSchemaType::Number: return serializer.readDouble(name);
			case AgentBehaviourSchemaType::String: return serializer.readString(name);
			case AgentBehaviourSchemaType::Duration:
				return AgentBehaviourDuration{ serializer.readUint64(name) };
			case AgentBehaviourSchemaType::Marker:
				return MarkerId{ serializer.readUint64(name) };
			case AgentBehaviourSchemaType::List:
			{
				if (field.children.size() != 1)
					throw SerializationException(
						"An Agent behaviour List default has no element schema");
				AgentBehaviourConfigurationList list;
				serializer.beginArray(name);
				while (serializer.nextArrayItem())
				{
					if (list.size() >= MaxAgentBehaviourListElements)
						throw SerializationException(
							"An Agent behaviour List default exceeds 4096 elements");
					serializer.beginMap("");
					auto const type = serializer.readString("type");
					if (type != agentBehaviourSchemaTypeName(field.children.front().type))
						throw SerializationException("An Agent behaviour List default element has type '"
							+ type + "', expected '"
							+ agentBehaviourSchemaTypeName(field.children.front().type) + "'");
					list.push_back(deserializeConfigurationValue(serializer,
						field.children.front(), "value", depth + 1));
					serializer.endMap();
				}
				serializer.endArray();
				return list;
			}
			case AgentBehaviourSchemaType::Record:
			{
				AgentBehaviourConfigurationRecord record;
				serializer.beginArray(name);
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					auto const nestedName = serializer.readString("field");
					auto child = find_if(field.children.begin(), field.children.end(),
						[&](auto const& candidate) { return candidate.name == nestedName; });
					if (child == field.children.end())
						throw SerializationException("An Agent behaviour Record default contains undeclared field '"
							+ nestedName + "'");
					auto const type = serializer.readString("type");
					if (type != agentBehaviourSchemaTypeName(child->type))
						throw SerializationException("Agent behaviour Record default field '"
							+ nestedName + "' has type '" + type + "', expected '"
							+ agentBehaviourSchemaTypeName(child->type) + "'");
					auto value = deserializeConfigurationValue(serializer, *child,
						"value", depth + 1);
					serializer.endMap();
					if (!record.emplace(nestedName, std::move(value)).second)
						throw SerializationException("Agent behaviour Record default field '"
							+ nestedName + "' appears twice");
				}
				serializer.endArray();
				return record;
			}
			}
			throw SerializationException("Unknown Agent behaviour configuration type");
		}

		void serializeSchemaField(Serializer& serializer,
			AgentBehaviourSchemaField const& field)
		{
			serializer.beginMap("");
			serializer.writeString("name", field.name);
			serializer.writeString("type", agentBehaviourSchemaTypeName(field.type));
			if (!field.required) serializer.writeBool("required", false);
			if (field.defaultValue)
				serializeConfigurationValue(serializer, *field.defaultValue, "default");
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
			if (serializer.hasField("children"))
			{
				serializer.beginArray("children");
				while (serializer.nextArrayItem())
					field.children.push_back(deserializeSchemaField(serializer, depth + 1));
				serializer.endArray();
			}
			if (serializer.hasField("default"))
				field.defaultValue = deserializeConfigurationValue(serializer, field,
					"default", depth);
			serializer.endMap();
			return field;
		}

		void appendSchemaChanges(
			std::vector<AgentBehaviourSchemaField> const& previous,
			std::vector<AgentBehaviourSchemaField> const& candidate,
			std::string const& prefix,
			std::vector<AgentBehaviourSchemaFieldChange>& changes,
			bool& compatible)
		{
			auto add = [&](std::string path,
				AgentBehaviourSchemaCompatibility classification,
				std::string diagnostic)
			{
				changes.push_back({ std::move(path), classification,
					std::move(diagnostic) });
				if (classification == AgentBehaviourSchemaCompatibility::Incompatible)
					compatible = false;
			};
			for (auto const& oldField : previous)
			{
				auto const path = prefix.empty() ? oldField.name
					: prefix + "." + oldField.name;
				auto found = std::find_if(candidate.begin(), candidate.end(),
					[&](auto const& field) { return field.name == oldField.name; });
				if (found == candidate.end())
				{
					add(path, AgentBehaviourSchemaCompatibility::Incompatible,
						"field was removed");
					continue;
				}
				if (found->type != oldField.type)
				{
					add(path, AgentBehaviourSchemaCompatibility::Incompatible,
						std::format("type changed from {} to {}",
							agentBehaviourSchemaTypeName(oldField.type),
							agentBehaviourSchemaTypeName(found->type)));
					continue;
				}
				if (found->required != oldField.required
					|| found->defaultValue != oldField.defaultValue)
				{
					add(path, AgentBehaviourSchemaCompatibility::Incompatible,
						found->required && !oldField.required
							? "field became required"
							: "field requirement or default changed");
				}
				if (oldField.type == AgentBehaviourSchemaType::Record)
					appendSchemaChanges(oldField.children, found->children,
						path, changes, compatible);
				else if (oldField.type == AgentBehaviourSchemaType::List
					&& oldField.children.size() == 1
					&& found->children.size() == 1)
				{
					auto const& oldElement = oldField.children.front();
					auto const& newElement = found->children.front();
					auto const elementPath = path + "[]";
					if (oldElement.type != newElement.type)
						add(elementPath,
							AgentBehaviourSchemaCompatibility::Incompatible,
							std::format("element type changed from {} to {}",
								agentBehaviourSchemaTypeName(oldElement.type),
								agentBehaviourSchemaTypeName(newElement.type)));
					else if (oldElement.type == AgentBehaviourSchemaType::Record
						|| oldElement.type == AgentBehaviourSchemaType::List)
						appendSchemaChanges(oldElement.children, newElement.children,
							elementPath, changes, compatible);
				}
			}
			for (auto const& newField : candidate)
			{
				auto found = std::find_if(previous.begin(), previous.end(),
					[&](auto const& field) { return field.name == newField.name; });
				if (found != previous.end()) continue;
				auto const path = prefix.empty() ? newField.name
					: prefix + "." + newField.name;
				if (!newField.required && newField.defaultValue)
					add(path, AgentBehaviourSchemaCompatibility::Compatible,
						"optional field will be materialized from its default");
				else
					add(path, AgentBehaviourSchemaCompatibility::Incompatible,
						"required field was introduced");
			}
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
		// deserialization; here they must also exist beside the manifest. One
		// deterministic pass caches and preflights the complete declared graph.
		registry->mPackageDirectory = packageDirectory;
		registry->preflightPackage(packageDirectory);
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

	uint64_t AgentBehaviourRegistry::getPackageRevision() const
	{
		return mPackageRevision;
	}

	std::vector<std::string> AgentBehaviourRegistry::getHelperModuleNames() const
	{
		std::vector<std::string> names;
		names.reserve(mHelperModules.size());
		for (auto const& [name, module] : mHelperModules)
		{
			(void)module;
			names.push_back(name);
		}
		return names;
	}

	AgentBehaviourHelperModule const* AgentBehaviourRegistry::lookupHelperModule(
		std::string const& name) const
	{
		auto found = mHelperModules.find(name);
		return found == mHelperModules.end() ? nullptr : found->second.get();
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
		if (std::find(mLoadedBuildings.begin(), mLoadedBuildings.end(), &building)
			== mLoadedBuildings.end())
			mLoadedBuildings.push_back(&building);
	}

	void AgentBehaviourRegistry::unregisterBuilding(Building& building)
	{
		std::erase(mLoadedBuildings, &building);
	}

	bool AgentBehaviourRegistry::hasLoadedBuilding(Building const* building) const
	{
		return building && std::find(mLoadedBuildings.begin(), mLoadedBuildings.end(),
			building) != mLoadedBuildings.end();
	}

	bool AgentBehaviourRegistry::hasLoadedBuildings() const
	{
		return !mLoadedBuildings.empty();
	}

	std::vector<LoadedAgentBehaviourUsage>
	AgentBehaviourRegistry::getLoadedAgentBehaviourUsage(AgentBehaviourId id) const
	{
		if (!mBehaviours.find(id))
			throw std::out_of_range(std::format(
				"Agent behaviour {} is not defined in this registry", id.value));
		std::vector<LoadedAgentBehaviourUsage> result;
		for (auto const* building : mLoadedBuildings)
		{
			if (!building) continue;
			LoadedAgentBehaviourUsage usage;
			usage.building = building;
			for (auto const& [agentId, agent] : building->mAgents.entries())
			{
				if (agent && agent->getBehaviourAssignment()
					&& agent->getBehaviourAssignment()->behaviour == id)
					usage.agents.push_back({ agentId, agent->getName() });
			}
			if (!usage.agents.empty()) result.push_back(std::move(usage));
		}
		std::sort(result.begin(), result.end(), [](auto const& left, auto const& right)
		{
			if (left.building->getName() != right.building->getName())
				return left.building->getName() < right.building->getName();
			return left.building < right.building;
		});
		return result;
	}

	uint64_t AgentBehaviourRegistry::getLoadedAgentBehaviourUsageCount(
		AgentBehaviourId id) const
	{
		uint64_t count{ 0 };
		for (auto const& usage : getLoadedAgentBehaviourUsage(id))
			count += usage.agents.size();
		return count;
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

	bool AgentBehaviourRegistry::previewDefinitionsFrom(
		AgentBehaviourRegistry& replacement,
		AgentBehaviourSchemaMigrationPreview& preview,
		std::string* diagnostic) const
	{
		preview = {};
		auto reject = [diagnostic](std::string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (replacement.mUuid != mUuid)
			return reject(std::format(
				"Agent behaviour registry UUID mismatch: loaded {}, file contains {}",
				mUuid, replacement.mUuid));

		// Carry trusted historical schemas forward before examining assignments.
		// A candidate may omit history (external authors normally edit only the
		// current schema), but it may never rewrite a history already known here.
		for (auto const& [id, oldDefinition] : mBehaviours.entries())
		{
			auto* nextDefinition = replacement.mBehaviours.find(id);
			if (!nextDefinition) continue;
			for (auto const& [revision, schema] : oldDefinition->getSchemaHistory())
			{
				if (auto const* supplied = nextDefinition->getSchemaAtRevision(revision);
					supplied && *supplied != schema)
					return reject(std::format(
						"Agent behaviour {} schema history revision {} was rewritten",
						id.value, revision));
				if (!nextDefinition->getSchemaAtRevision(revision))
					nextDefinition->rememberSchema(revision, schema);
			}
		}

		auto buildings = mLoadedBuildings;
		std::stable_sort(buildings.begin(), buildings.end(),
			[](Building const* left, Building const* right)
			{
				if (!left || !right) return left != nullptr;
				return left->getName() < right->getName();
			});
		for (auto* building : buildings)
		{
			if (!building) continue;
			for (auto const& [agentId, agent] : building->mAgents.entries())
			{
				if (!agent || !agent->getBehaviourAssignment()) continue;
				auto const& assignment = *agent->getBehaviourAssignment();
				auto const* next = replacement.lookupAgentBehaviour(
					assignment.behaviour);
				if (next && assignment.revision == next->getRevision())
				{
					std::string validation;
					if (building->validateAgentBehaviourAssignmentAgainst(replacement,
						assignment.behaviour, assignment.revision,
						assignment.configuration, nullptr, &validation)) continue;
				}

				AgentBehaviourSchemaMigrationItem item;
				item.building = building;
				item.buildingName = building->getName();
				item.agent = agentId;
				item.agentName = agent->getName();
				item.behaviour = assignment.behaviour;
				item.fromRevision = assignment.revision;
				item.toRevision = next ? next->getRevision() : 0;
				item.behaviourName = next ? next->getName()
					: std::format("{}", assignment.behaviour.value);
				bool compatible = true;
				if (!next)
				{
					compatible = false;
					item.fields.push_back({ "$behaviour",
						AgentBehaviourSchemaCompatibility::Incompatible,
						"behaviour was removed" });
				}
				else if (assignment.revision > next->getRevision())
				{
					compatible = false;
					item.fields.push_back({ "$revision",
						AgentBehaviourSchemaCompatibility::Incompatible,
						"candidate revision is older than the validated assignment" });
				}
				else if (auto const* previous = next->getSchemaAtRevision(
					assignment.revision))
				{
					appendSchemaChanges(*previous, next->getSchema(), {},
						item.fields, compatible);
				}
				else
				{
					compatible = false;
					item.fields.push_back({ "$revision",
						AgentBehaviourSchemaCompatibility::Incompatible,
						"validated schema revision is not retained by the registry" });
				}

				AgentBehaviourConfiguration normalized;
				std::string validation;
				if (compatible && !building->validateAgentBehaviourAssignmentAgainst(
					replacement, assignment.behaviour, next->getRevision(),
					assignment.configuration, &normalized, &validation))
				{
					compatible = false;
					item.fields.push_back({ "$configuration",
						AgentBehaviourSchemaCompatibility::Incompatible,
						std::move(validation) });
				}
				item.compatibility = compatible
					? AgentBehaviourSchemaCompatibility::Compatible
					: AgentBehaviourSchemaCompatibility::Incompatible;
				preview.requiresExplicitMigration |= !compatible;
				preview.configurations.push_back(std::move(item));
			}
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentBehaviourRegistry::replaceDefinitionsFrom(AgentBehaviourRegistry&& replacement,
		std::string* diagnostic,
		std::vector<AgentBehaviourReloadDiagnostic>* reloadDiagnostics,
		std::vector<AgentBehaviourConfigurationMigration> const& migrations)
	{
		if (reloadDiagnostics) reloadDiagnostics->clear();
		auto reject = [diagnostic, reloadDiagnostics](std::string message)
		{
			if (diagnostic) *diagnostic = message;
			if (reloadDiagnostics) reloadDiagnostics->push_back({
				AgentBehaviourReloadDiagnosticScope::Package, {}, {}, {}, {}, {}, {},
				message, {} });
			return false;
		};
		if (replacement.mUuid != mUuid)
		{
			return reject(std::format(
				"Agent behaviour registry UUID mismatch: loaded {}, file contains {}",
				mUuid, replacement.mUuid));
		}
		if (!definitionEditsAreAllowed(diagnostic))
		{
			if (reloadDiagnostics && diagnostic)
				reloadDiagnostics->push_back({
					AgentBehaviourReloadDiagnosticScope::Package, {}, {}, {}, {}, {}, {},
					*diagnostic, {} });
			return false;
		}

		if (replacement.mPackageRevision < mPackageRevision)
			return reject("Agent behaviour registry package revision cannot move backwards on reload");
		auto helperDefinitionsEqual = [&]
		{
			if (mHelperModules.size() != replacement.mHelperModules.size()) return false;
			auto left = mHelperModules.begin();
			auto right = replacement.mHelperModules.begin();
			for (; left != mHelperModules.end(); ++left, ++right)
			{
				if (left->first != right->first
					|| !left->second->definitionEquals(*right->second)) return false;
			}
			return true;
		};
		if (!helperDefinitionsEqual()
			&& replacement.mPackageRevision == mPackageRevision)
			return reject("Changed helper-module dependencies require an increasing package revision");

		if (mBehaviours.nextId() == 0 ? replacement.mBehaviours.nextId() != 0
			: replacement.mBehaviours.nextId() != 0
				&& replacement.mBehaviours.nextId() < mBehaviours.nextId())
			return reject("Agent behaviour allocator cannot move backwards on reload");
		for (auto const& [id, candidateDefinition] : replacement.mBehaviours.entries())
		{
			auto const* previous = mBehaviours.find(id);
			if (!previous && (mBehaviours.nextId() == 0 || id.value < mBehaviours.nextId()))
				return reject("Agent behaviour reload cannot reuse a deleted ID");
			if (previous && (candidateDefinition->getRevision() < previous->getRevision()
				|| ((candidateDefinition->getSchema() != previous->getSchema()
					|| candidateDefinition->getSourceModulePath() != previous->getSourceModulePath())
					&& candidateDefinition->getRevision() == previous->getRevision())))
				return reject("Changed behaviour definitions require an increasing revision");
		}

		std::vector<AgentBehaviourReloadDiagnostic> failures;
		for (auto const& [name, helper] : replacement.mHelperModules)
		{
			if (helper->getModuleStatus() != AgentBehaviourModuleStatus::Error) continue;
			failures.push_back({ AgentBehaviourReloadDiagnosticScope::Module,
				{}, {}, {}, {}, {}, helper->getSourceModulePath(),
				helper->getModuleDiagnostic(), helper->getModuleTraceback() });
		}
		for (auto const& [id, behaviour] : replacement.mBehaviours.entries())
		{
			if (behaviour->getModuleStatus() != AgentBehaviourModuleStatus::Error) continue;
			failures.push_back({ AgentBehaviourReloadDiagnosticScope::Module,
				{}, {}, {}, behaviour->getName(), id,
				behaviour->getSourceModulePath(), behaviour->getModuleDiagnostic(),
				behaviour->getModuleTraceback() });
		}
		if (!failures.empty())
		{
			if (reloadDiagnostics) *reloadDiagnostics = std::move(failures);
			if (diagnostic) *diagnostic = std::format(
				"Agent behaviour reload preflight found {} module diagnostic(s)",
				reloadDiagnostics ? reloadDiagnostics->size() : failures.size());
			return false;
		}

		bool schemaHistoryNeedsSave{ false };
		for (auto const& [id, previous] : mBehaviours.entries())
		{
			auto const* candidate = replacement.mBehaviours.find(id);
			if (candidate && candidate->getRevision() > previous->getRevision()
				&& candidate->getSchema() != previous->getSchema())
				schemaHistoryNeedsSave = true;
		}
		AgentBehaviourSchemaMigrationPreview preview;
		std::string previewDiagnostic;
		if (!previewDefinitionsFrom(replacement, preview, &previewDiagnostic))
			return reject(std::move(previewDiagnostic));

		auto buildings = mLoadedBuildings;
		std::stable_sort(buildings.begin(), buildings.end(),
			[](Building const* left, Building const* right)
			{
				if (!left || !right) return left != nullptr;
				return left->getName() < right->getName();
			});
		std::map<Building*, std::map<AgentId, AgentBehaviourAssignment>>
			assignmentPlans;
		std::set<std::pair<Building*, AgentId>> usedMigrations;
		for (auto const& item : preview.configurations)
		{
			auto* building = item.building;
			if (!building) continue;
			auto const* agent = building->mAgents.find(item.agent);
			if (!agent || !agent->getBehaviourAssignment())
				return reject("An affected Agent closed during schema classification");
			auto const* next = replacement.lookupAgentBehaviour(item.behaviour);

			AgentBehaviourConfiguration sourceConfiguration
				= agent->getBehaviourAssignment()->configuration;
			if (item.compatibility == AgentBehaviourSchemaCompatibility::Incompatible)
			{
				auto migration = std::find_if(migrations.begin(), migrations.end(),
					[&](auto const& candidate)
					{
						return candidate.building == building
							&& candidate.agent == item.agent;
					});
				if (migration == migrations.end())
				{
					std::string fields;
					for (auto const& field : item.fields)
						fields += (fields.empty() ? "" : ", ") + field.path
							+ " (" + field.diagnostic + ")";
					failures.push_back({ AgentBehaviourReloadDiagnosticScope::Agent,
						item.buildingName, item.agentName, item.agent,
						item.behaviourName, item.behaviour, {},
						"Explicit coordinated migration required for " + fields, {} });
					continue;
				}
				if (!usedMigrations.emplace(building, item.agent).second)
					return reject("An Agent configuration migration appears more than once");
				sourceConfiguration = migration->configuration;
			}
			if (!next)
			{
				failures.push_back({ AgentBehaviourReloadDiagnosticScope::Agent,
					item.buildingName, item.agentName, item.agent,
					item.behaviourName, item.behaviour, {},
					"A removed behaviour cannot receive a configuration migration", {} });
				continue;
			}
			AgentBehaviourConfiguration normalized;
			std::string assignmentDiagnostic;
			if (!building->validateAgentBehaviourAssignmentAgainst(replacement,
				item.behaviour, next->getRevision(), sourceConfiguration,
				&normalized, &assignmentDiagnostic))
			{
				failures.push_back({ AgentBehaviourReloadDiagnosticScope::Agent,
					item.buildingName, item.agentName, item.agent,
					item.behaviourName, item.behaviour, {},
					std::move(assignmentDiagnostic), {} });
				continue;
			}
			assignmentPlans[building][item.agent] = AgentBehaviourAssignment{
				item.behaviour, next->getRevision(), std::move(normalized) };
		}
		for (auto const& migration : migrations)
		{
			if (!migration.building || !usedMigrations.contains(
				{ migration.building, migration.agent }))
				return reject("A supplied Agent configuration migration is not required by the preview");
		}
		if (!failures.empty())
		{
			if (reloadDiagnostics) *reloadDiagnostics = std::move(failures);
			if (diagnostic) *diagnostic = std::format(
				"Agent behaviour schema reconciliation found {} configuration diagnostic(s)",
				reloadDiagnostics ? reloadDiagnostics->size() : failures.size());
			return false;
		}

		struct PreparedBuilding
		{
			Building* building{ nullptr };
			std::unique_ptr<AgentBehaviourRuntimeAdapter> runtime;
			std::map<AgentId, AgentBehaviourAssignment> assignments;
		};
		std::vector<PreparedBuilding> preparedBuildings;
		preparedBuildings.reserve(buildings.size());
		for (auto* building : buildings)
		{
			if (!building) continue;
			std::unique_ptr<AgentBehaviourRuntimeAdapter> candidateRuntime;
			std::vector<AgentBehaviourRuntimeDiagnostic> runtimeDiagnostics;
			auto plan = assignmentPlans.find(building);
			auto const* overrides = plan == assignmentPlans.end()
				? nullptr : &plan->second;
			if (!AgentBehaviourRuntimeAdapter::prepareReload(*building, replacement,
				candidateRuntime, runtimeDiagnostics, overrides))
			{
				for (auto const& runtimeDiagnostic : runtimeDiagnostics)
					failures.push_back({ AgentBehaviourReloadDiagnosticScope::Agent,
						building->getName(), runtimeDiagnostic.agentName,
						runtimeDiagnostic.agent, runtimeDiagnostic.behaviourName,
						runtimeDiagnostic.behaviour, runtimeDiagnostic.moduleName,
						runtimeDiagnostic.diagnostic, runtimeDiagnostic.traceback });
				continue;
			}
			preparedBuildings.push_back({ building, std::move(candidateRuntime),
				plan == assignmentPlans.end()
					? std::map<AgentId, AgentBehaviourAssignment>{}
					: std::move(plan->second) });
		}
		if (!failures.empty())
		{
			if (reloadDiagnostics) *reloadDiagnostics = std::move(failures);
			if (diagnostic) *diagnostic = std::format(
				"Agent behaviour reload preflight found {} per-Agent factory diagnostic(s)",
				reloadDiagnostics ? reloadDiagnostics->size() : failures.size());
			return false;
		}

		// No operation from here can veto the transaction. Definitions and every
		// already-constructed candidate runtime become the new revision together;
		// old instances receive only their read-only best-effort teardown first.
		mPackageRevision = replacement.mPackageRevision;
		mHelperModules = std::move(replacement.mHelperModules);
		mBehaviours = std::move(replacement.mBehaviours);
		mSourceCache = std::move(replacement.mSourceCache);
		mPackageDirectory = std::move(replacement.mPackageDirectory);
		mDocumentPath = std::move(replacement.mDocumentPath);
		mSavedDocumentContents = std::move(replacement.mSavedDocumentContents);
		for (auto& prepared : preparedBuildings)
		{
			auto* building = prepared.building;
			building->mAgentBehaviourRuntime->teardownAll(*building,
				AgentBehaviourTeardownReason::Reload);
			auto teardownDiagnostics = building->mAgentBehaviourRuntime
				->consumeDiagnostics();
			prepared.runtime->appendDiagnostics(std::move(teardownDiagnostics));
			building->mAgentBehaviourRuntime = std::move(prepared.runtime);
			for (auto& [agentId, assignment] : prepared.assignments)
			{
				auto* agent = building->mAgents.find(agentId);
				if (agent) agent->setBehaviourAssignment(std::move(assignment));
			}
			if (!prepared.assignments.empty()) building->modify();
			building->mAgentBehaviourDependencyDiagnostic.clear();
			for (auto const& [agentId, agent] : building->mAgents.entries())
				if (agent && agent->getBehaviourAssignment())
					building->mSimulationCoordinator
						.clearAgentMovementForBehaviourEdit(agentId);
		}
		if (schemaHistoryNeedsSave) modify();
		else markUnmodified();
		if (reloadDiagnostics) reloadDiagnostics->clear();
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

	void AgentBehaviourRegistry::preflightPackage(
		std::filesystem::path const& packageDirectory)
	{
		mSourceCache.clear();
		auto cacheSource = [&](std::string const& path)
		{
			if (mSourceCache.contains(path)) return;
			requireModuleFile(path, packageDirectory);
			std::ifstream input(packageDirectory / path, std::ios::binary);
			if (!input)
				throw SerializationException(std::format(
					"Could not read Agent behaviour package module {}", path));
			mSourceCache.emplace(path, std::string(
				std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
		};

		for (auto const& [name, helper] : mHelperModules)
		{
			(void)name;
			cacheSource(helper->getSourceModulePath());
		}
		for (auto const& [id, behaviour] : mBehaviours.entries())
		{
			(void)id;
			cacheSource(behaviour->getSourceModulePath());
		}

		std::vector<AgentBehaviourHelperSource> helpers;
		helpers.reserve(mHelperModules.size());
		for (auto const& [name, helper] : mHelperModules)
			helpers.push_back({ name, helper->getSourceModulePath(),
				mSourceCache.at(helper->getSourceModulePath()) });

		auto const packageName = packageDirectory.filename().string();
		for (auto& [name, helper] : mHelperModules)
		{
			auto result = AgentBehaviourRuntimeAdapter::preflightHelperModule(
				packageName, name, helper->getSourceModulePath(),
				mSourceCache.at(helper->getSourceModulePath()), helpers);
			helper->setModulePreflight(
				result.loaded ? AgentBehaviourModuleStatus::Loaded
					: AgentBehaviourModuleStatus::Error,
				std::move(result.diagnostic), std::move(result.traceback));
		}
		for (auto const& [id, behaviour] : mBehaviours.entries())
		{
			(void)id;
			auto result = behaviour->getSchema().empty()
				? AgentBehaviourRuntimeAdapter::preflightModule(
					packageName, behaviour->getSourceModulePath(),
					mSourceCache.at(behaviour->getSourceModulePath()), helpers)
				: AgentBehaviourRuntimeAdapter::preflightModuleContract(
					packageName, behaviour->getSourceModulePath(),
					mSourceCache.at(behaviour->getSourceModulePath()), helpers);
			behaviour->setModulePreflight(
				result.loaded ? AgentBehaviourModuleStatus::Loaded
					: AgentBehaviourModuleStatus::Error,
				std::move(result.diagnostic), std::move(result.traceback));
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
		if (mPackageDirectory) preflightPackage(*mPackageDirectory);
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

	bool AgentBehaviourRegistry::deleteAgentBehaviourClearingAssignments(
		AgentBehaviourId id, std::string* diagnostic)
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

		struct PreparedBuilding
		{
			Building* building{ nullptr };
			std::vector<AgentId> agents;
			std::unique_ptr<AgentBehaviourRuntimeAdapter> runtime;
		};
		std::vector<PreparedBuilding> prepared;
		for (auto const& usage : getLoadedAgentBehaviourUsage(id))
		{
			auto* building = const_cast<Building*>(usage.building);
			if (!building || building->mAgentBehaviourRegistry.get() != this)
				return reject("An affected Building can no longer participate in Agent behaviour deletion");
			if (!building->agentBehaviourConfigurationsAreValid())
				return reject(std::format(
					"Building '{}' cannot participate: {}", building->getName(),
					building->getAgentBehaviourDependencyDiagnostic()));
			PreparedBuilding item;
			item.building = building;
			for (auto const& agent : usage.agents) item.agents.push_back(agent.id);
			std::vector<AgentBehaviourRuntimeDiagnostic> failures;
			if (!AgentBehaviourRuntimeAdapter::prepareReload(*building, *this,
				item.runtime, failures, nullptr, id))
			{
				std::string details;
				for (auto const& failure : failures)
					details += (details.empty() ? "" : "\n") + failure.diagnostic;
				return reject(std::format(
					"Building '{}' cannot participate in Agent behaviour deletion{}{}",
					building->getName(), details.empty() ? "" : ":\n", details));
			}
			prepared.push_back(std::move(item));
		}

		// All refusing and allocating work is complete. Teardown callbacks are
		// best-effort and cannot veto this commit.
		mBehaviours.remove(id);
		modify();
		for (auto& item : prepared)
		{
			auto& building = *item.building;
			building.mAgentBehaviourRuntime->teardownAll(building,
				AgentBehaviourTeardownReason::BehaviourDeletion);
			item.runtime->appendDiagnostics(
				building.mAgentBehaviourRuntime->consumeDiagnostics());
			building.mAgentBehaviourRuntime = std::move(item.runtime);
			for (auto agentId : item.agents)
			{
				building.mSimulationCoordinator.clearAgentMovementForBehaviourEdit(agentId);
				if (auto* agent = building.mAgents.find(agentId))
					agent->clearBehaviourAssignment();
			}
			building.modify();
		}
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
		serializer.writeUint64("revision", mPackageRevision);
		serializer.beginArray("modules");
		for (auto const& [name, module] : mHelperModules)
		{
			serializer.beginMap("");
			serializer.writeString("name", name);
			serializer.writeString("source", module->getSourceModulePath());
			serializer.endMap();
		}
		serializer.endArray();
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
			bool hasHistoricalSchema{ false };
			for (auto const& [revision, schema] : behaviour->getSchemaHistory())
			{
				(void)schema;
				if (revision != behaviour->getRevision()) hasHistoricalSchema = true;
			}
			if (hasHistoricalSchema)
			{
				serializer.beginArray("schemaHistory");
				for (auto const& [revision, schema] : behaviour->getSchemaHistory())
				{
					if (revision == behaviour->getRevision()) continue;
					serializer.beginMap("");
					serializer.writeUint64("revision", revision);
					serializer.beginArray("schema");
					for (auto const& field : schema)
						serializeSchemaField(serializer, field);
					serializer.endArray();
					serializer.endMap();
				}
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
		auto const packageRevision = serializer.readUint64("revision", true, 1);
		if (packageRevision == 0)
			throw SerializationException("Agent behaviour registry package revision cannot be zero");

		std::map<std::string, std::unique_ptr<AgentBehaviourHelperModule>> helperModules;
		if (serializer.hasField("modules"))
		{
			serializer.beginArray("modules");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				auto name = serializer.readString("name");
				auto source = serializer.readString("source");
				serializer.endMap();
				std::string diagnostic;
				if (!AgentBehaviourHelperModule::nameIsValid(name, &diagnostic))
					throw SerializationException(
						"Serialized helper module name is invalid: " + diagnostic);
				if (!AgentBehaviour::sourceModulePathIsValid(source, &diagnostic))
					throw SerializationException(
						"Serialized helper module source path is invalid: " + diagnostic);
				if (!helperModules.emplace(name,
					AgentBehaviourHelperModule::create(name, std::move(source))).second)
					throw SerializationException(std::format(
						"Serialized helper module names must be unique ('{}' appears twice)", name));
			}
			serializer.endArray();
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
			std::map<uint64_t, std::vector<AgentBehaviourSchemaField>> schemaHistory;
			if (serializer.hasField("schemaHistory"))
			{
				serializer.beginArray("schemaHistory");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					auto const historicalRevision = serializer.readUint64("revision");
					std::vector<AgentBehaviourSchemaField> historicalSchema;
					serializer.beginArray("schema");
					while (serializer.nextArrayItem())
						historicalSchema.push_back(deserializeSchemaField(serializer));
					serializer.endArray();
					serializer.endMap();
					if (!historicalRevision || historicalRevision >= revision
						|| !schemaHistory.emplace(historicalRevision,
							std::move(historicalSchema)).second)
						throw SerializationException(
							"Agent behaviour schema history revisions must be unique, nonzero, and older than the current revision");
				}
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
			for (auto const& [historicalRevision, historicalSchema] : schemaHistory)
			{
				(void)historicalRevision;
				if (!agentBehaviourSchemaFieldsAreValid(historicalSchema, &diagnostic))
					throw SerializationException(
						"Serialized historical Agent behaviour schema is invalid: "
						+ diagnostic);
			}
			auto definition = AgentBehaviour::create(std::move(name),
				std::move(source), std::move(schema), revision);
			for (auto& [historicalRevision, historicalSchema] : schemaHistory)
				definition->rememberSchema(historicalRevision,
					std::move(historicalSchema));
			if (!behaviours.restore(id, std::move(definition)))
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
		mPackageRevision = packageRevision;
		mHelperModules = std::move(helperModules);
		mBehaviours = std::move(behaviours);
		return true;
	}

	void AgentBehaviourRegistry::saveTo(std::string const& manifestFilepath)
	{
		for (auto const* building : mLoadedBuildings)
		{
			if (!building) continue;
			std::string dependencyDiagnostic;
			if (!building->agentBehaviourConfigurationsAreValid()
				|| !building->inspectAgentBehaviourAssignments(*this,
					&dependencyDiagnostic))
			{
				if (dependencyDiagnostic.empty())
					dependencyDiagnostic = building->getAgentBehaviourDependencyDiagnostic();
				throw SerializationException(std::format(
					"Cannot save Agent behaviour registry while Building '{}' has an invalid dependent configuration: {}",
					building->getName(), dependencyDiagnostic));
			}
		}
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

		for (auto const& [name, helper] : mHelperModules)
		{
			(void)name;
			requireModuleFile(helper->getSourceModulePath(), path.parent_path());
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
