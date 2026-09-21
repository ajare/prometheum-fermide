#include "core/AgentTagRegistryDocument.h"

#include <algorithm>
#include <format>
#include <optional>
#include <system_error>
#include <vector>

#include "core/AgentTagRegistry.h"
#include "core/Building.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"

namespace core
{
	namespace
	{
		struct LoadedAgentTagRegistry
		{
			std::filesystem::path canonicalPath;
			std::weak_ptr<AgentTagRegistry> registry;
		};

		// Weak ownership lets a clean registry loaded only for a failed operation
		// disappear with that operation. Buildings (and, in later tickets, dirty
		// registry documents) provide the strong ownership while it is in use.
		std::vector<LoadedAgentTagRegistry> gLoadedAgentTagRegistries;

		void discardUnreferencedRegistries()
		{
			std::erase_if(gLoadedAgentTagRegistries,
				[](LoadedAgentTagRegistry const& entry) { return entry.registry.expired(); });
		}

		bool pathsReferToSameFile(std::filesystem::path const& left,
			std::filesystem::path const& right)
		{
			// Registry identity is its canonical absolute path. A hard link under a
			// second canonical path is therefore a second path and is caught by the
			// duplicate-UUID rule below rather than silently becoming an alias.
			return left == right;
		}

		std::filesystem::path requireCanonicalRegularFile(
			std::filesystem::path const& filepath, char const* documentName)
		{
			if (filepath.empty())
			{
				throw SerializationException(std::format(
					"{} file path is empty", documentName));
			}

			std::error_code error;
			auto canonical = std::filesystem::canonical(filepath, error);
			if (error || !std::filesystem::is_regular_file(canonical, error) || error)
			{
				throw SerializationException(std::format(
					"{} file is missing or is not a regular file: {}",
					documentName, filepath.string()));
			}
			return canonical;
		}

		std::filesystem::path requireSavedBuildingPath(
			std::filesystem::path const& buildingFilepath)
		{
			try
			{
				return requireCanonicalRegularFile(buildingFilepath, "Building");
			}
			catch (SerializationException const&)
			{
				throw SerializationException(
					"Save the Building before creating, selecting, or loading an Agent tag registry");
			}
		}

		void requireAgentTagRegistryFilename(std::filesystem::path const& filepath)
		{
			auto const filename = filepath.filename().string();
			if (filename.empty() || !filename.ends_with(".tags.yaml"))
			{
				throw SerializationException(
					"An Agent tag registry file must end with .tags.yaml");
			}
		}

		std::shared_ptr<AgentTagRegistry> readRegistry(
			std::filesystem::path const& canonicalPath)
		{
			try
			{
				return AgentTagRegistry::loadFrom(canonicalPath.string());
			}
			catch (std::exception const& error)
			{
				throw SerializationException(std::format(
					"Could not load Agent tag registry {}: {}",
					canonicalPath.string(), error.what()));
			}
		}

		void requireExpectedUuid(AgentTagRegistry const& registry,
			std::optional<std::string> const& expectedUuid)
		{
			if (expectedUuid && registry.getUuid() != *expectedUuid)
			{
				throw SerializationException(std::format(
					"Agent tag registry UUID mismatch: Building expects {}, file contains {}",
					*expectedUuid, registry.getUuid()));
			}
		}

		std::shared_ptr<AgentTagRegistry> loadSharedRegistry(
			std::filesystem::path const& registryFilepath,
			std::optional<std::string> const& expectedUuid = std::nullopt)
		{
			requireAgentTagRegistryFilename(registryFilepath);
			auto const canonicalPath = requireCanonicalRegularFile(
				registryFilepath, "Agent tag registry");
			requireAgentTagRegistryFilename(canonicalPath);

			// Validate the file on every attachment, even when its loaded instance is
			// already shared. This catches deletion, substitution, malformed YAML, and
			// unsupported schemas without replacing any in-memory state.
			auto diskRegistry = readRegistry(canonicalPath);
			requireExpectedUuid(*diskRegistry, expectedUuid);

			discardUnreferencedRegistries();
			for (auto const& entry : gLoadedAgentTagRegistries)
			{
				auto loaded = entry.registry.lock();
				if (!loaded || !pathsReferToSameFile(entry.canonicalPath, canonicalPath))
					continue;
				if (loaded->getUuid() != diskRegistry->getUuid())
				{
					throw SerializationException(std::format(
						"Agent tag registry at {} was substituted: loaded UUID {}, file contains {}",
						canonicalPath.string(), loaded->getUuid(), diskRegistry->getUuid()));
				}
				return loaded;
			}

			for (auto const& entry : gLoadedAgentTagRegistries)
			{
				auto loaded = entry.registry.lock();
				if (loaded && loaded->getUuid() == diskRegistry->getUuid())
				{
					throw SerializationException(std::format(
						"Agent tag registry UUID {} is already loaded from another file: {} (refusing {})",
						diskRegistry->getUuid(), entry.canonicalPath.string(), canonicalPath.string()));
				}
			}

			gLoadedAgentTagRegistries.push_back({ canonicalPath, diskRegistry });
			return diskRegistry;
		}

		void registerCreatedRegistry(std::filesystem::path const& registryFilepath,
			std::shared_ptr<AgentTagRegistry> const& registry)
		{
			auto const canonicalPath = requireCanonicalRegularFile(
				registryFilepath, "Agent tag registry");
			discardUnreferencedRegistries();
			for (auto const& entry : gLoadedAgentTagRegistries)
			{
				auto loaded = entry.registry.lock();
				if (!loaded) continue;
				if (pathsReferToSameFile(entry.canonicalPath, canonicalPath))
				{
					throw SerializationException(std::format(
						"Agent tag registry is already loaded from {}",
						entry.canonicalPath.string()));
				}
				if (loaded->getUuid() == registry->getUuid())
				{
					throw SerializationException(std::format(
						"Agent tag registry UUID {} is already loaded from another file: {}",
						registry->getUuid(), entry.canonicalPath.string()));
				}
			}
			gLoadedAgentTagRegistries.push_back({ canonicalPath, registry });
		}
	}

	std::filesystem::path defaultAgentTagRegistryPath(
		std::filesystem::path const& buildingFilepath)
	{
		if (buildingFilepath.empty()) return {};
		auto filename = buildingFilepath.filename();
		filename.replace_extension();
		filename += ".tags.yaml";
		return buildingFilepath.parent_path() / filename;
	}

	std::shared_ptr<AgentTagRegistry> createAndAttachAgentTagRegistry(
		Building& building, std::filesystem::path const& buildingFilepath)
	{
		if (building.hasAgentTagRegistryReference())
		{
			throw SerializationException("The Building already references an Agent tag registry");
		}
		auto const savedBuilding = requireSavedBuildingPath(buildingFilepath);
		auto const registryPath = defaultAgentTagRegistryPath(savedBuilding);
		std::error_code error;
		if (std::filesystem::exists(registryPath, error) || error)
		{
			throw SerializationException(std::format(
				"Agent tag registry already exists: {}", registryPath.string()));
		}

		auto registry = AgentTagRegistry::create();
		// YamlSerializer installs the completed temporary file with one rename.
		// Attachment happens afterwards, so a failed write cannot leave the
		// Building referring to a partial or absent registry.
		registry->saveTo(registryPath.string());
		try
		{
			registerCreatedRegistry(registryPath, registry);
		}
		catch (...)
		{
			std::filesystem::remove(registryPath, error);
			throw;
		}
		building.attachAgentTagRegistry(registryPath.filename().string(), registry);
		return registry;
	}

	std::shared_ptr<AgentTagRegistry> selectAndAttachAgentTagRegistry(
		Building& building, std::filesystem::path const& buildingFilepath,
		std::filesystem::path const& registryFilepath)
	{
		if (building.hasAgentTagRegistryReference())
		{
			throw SerializationException("The Building already references an Agent tag registry");
		}
		requireAgentTagRegistryFilename(registryFilepath);
		auto const savedBuilding = requireSavedBuildingPath(buildingFilepath);
		auto const canonicalRegistry = requireCanonicalRegularFile(
			registryFilepath, "Agent tag registry");
		requireAgentTagRegistryFilename(canonicalRegistry);
		if (canonicalRegistry.parent_path() != savedBuilding.parent_path())
		{
			throw SerializationException(
				"An Agent tag registry must be in the same directory as its Building");
		}

		auto registry = loadSharedRegistry(canonicalRegistry);
		building.attachAgentTagRegistry(canonicalRegistry.filename().string(), registry);
		return registry;
	}

	std::shared_ptr<AgentTagRegistry> loadAndAttachAgentTagRegistry(
		Building& building, std::filesystem::path const& buildingFilepath)
	{
		if (!building.hasAgentTagRegistryReference()) return {};
		auto const savedBuilding = requireSavedBuildingPath(buildingFilepath);
		auto const registryPath = savedBuilding.parent_path()
			/ building.getAgentTagRegistryFilename();
		auto const canonicalRegistry = requireCanonicalRegularFile(
			registryPath, "Agent tag registry");
		requireAgentTagRegistryFilename(canonicalRegistry);
		if (canonicalRegistry.parent_path() != savedBuilding.parent_path())
		{
			throw SerializationException(
				"An Agent tag registry must be in the same directory as its Building");
		}
		auto registry = loadSharedRegistry(canonicalRegistry,
			building.getExpectedAgentTagRegistryUuid());
		building.resolveAgentTagRegistry(registry);
		return registry;
	}

	std::shared_ptr<Building> loadBuildingDocument(
		std::filesystem::path const& buildingFilepath)
	{
		auto const canonicalBuilding = requireCanonicalRegularFile(
			buildingFilepath, "Building");
		auto loaded = std::make_shared<Building>("Loading", 1, 1);
		auto serializer = YamlSerializer::fromFile(canonicalBuilding.string());
		serializer->deserialize();
		SerializationWorkData workData;
		if (!loaded->deserialize(*serializer, workData))
		{
			throw SerializationException("Could not deserialize Building");
		}
		loadAndAttachAgentTagRegistry(*loaded, canonicalBuilding);
		return loaded;
	}
}
