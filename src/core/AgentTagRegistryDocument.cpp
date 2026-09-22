#include "core/AgentTagRegistryDocument.h"

#include <algorithm>
#include <format>
#include <optional>
#include <system_error>
#include <vector>

#include "core/AgentTagRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
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
			std::shared_ptr<AgentTagRegistry> registry;
		};

		// The manager keeps dirty unreferenced documents alive so detaching one
		// Building cannot silently discard unsaved shared work. Clean documents are
		// removed as soon as no loaded Building references them.
		std::vector<LoadedAgentTagRegistry> gLoadedAgentTagRegistries;

		void discardUnreferencedRegistries()
		{
			std::erase_if(gLoadedAgentTagRegistries,
				[](LoadedAgentTagRegistry const& entry)
				{
					return !entry.registry || (!entry.registry->hasLoadedBuildings()
						&& !entry.registry->isModified());
				});
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
				auto const& loaded = entry.registry;
				if (!loaded || !pathsReferToSameFile(entry.canonicalPath, canonicalPath))
					continue;
				if (loaded->getUuid() != diskRegistry->getUuid())
				{
					throw SerializationException(std::format(
						"Agent tag registry at {} was substituted: loaded UUID {}, file contains {}",
						canonicalPath.string(), loaded->getUuid(), diskRegistry->getUuid()));
				}
				if (loaded->fileHasExternalChanges(canonicalPath.string()))
				{
					throw SerializationException(std::format(
						"Agent tag registry {} changed outside the editor; reload it before attaching another Building",
						canonicalPath.string()));
				}
				return loaded;
			}

			for (auto const& entry : gLoadedAgentTagRegistries)
			{
				auto const& loaded = entry.registry;
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
				auto const& loaded = entry.registry;
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

	std::shared_ptr<AgentTagRegistry> copyAgentTagRegistryDocument(
		AgentTagRegistry const& source,
		std::filesystem::path const& destinationFilepath)
	{
		requireAgentTagRegistryFilename(destinationFilepath);
		if (destinationFilepath.empty())
			throw SerializationException("The Agent tag registry copy path is empty");

		auto pathIsOccupied = [](std::filesystem::path const& path)
		{
			std::error_code error;
			auto const status = std::filesystem::symlink_status(path, error);
			if (!error) return status.type() != std::filesystem::file_type::not_found;
			if (error == std::errc::no_such_file_or_directory) return false;
			throw SerializationException(std::format(
				"Could not inspect Agent tag registry copy destination {}: {}",
				path.string(), error.message()));
		};
		if (pathIsOccupied(destinationFilepath))
		{
			throw SerializationException(std::format(
				"Agent tag registry copy already exists: {}",
				destinationFilepath.string()));
		}

		auto copy = AgentTagRegistry::copyWithNewUuid(source);
		auto directory = destinationFilepath.parent_path();
		if (directory.empty()) directory = ".";
		auto const stagingPath = directory
			/ (destinationFilepath.filename().string() + "." + copy->getUuid()
				+ ".copying.tmp");
		if (pathIsOccupied(stagingPath))
		{
			throw SerializationException(std::format(
				"Agent tag registry copy staging file already exists: {}",
				stagingPath.string()));
		}

		bool destinationInstalled{ false };
		try
		{
			copy->saveTo(stagingPath.string());

			// A hard-link installation is atomic and refuses an occupied name on all
			// supported platforms. Unlike rename, it cannot overwrite a destination
			// created after the collision preflight.
			std::error_code error;
			std::filesystem::create_hard_link(
				stagingPath, destinationFilepath, error);
			if (error)
			{
				throw SerializationException(std::format(
					"Could not install Agent tag registry copy at {} without overwriting it: {}",
					destinationFilepath.string(), error.message()));
			}
			destinationInstalled = true;
			std::filesystem::remove(stagingPath, error);

			auto persisted = readRegistry(requireCanonicalRegularFile(
				destinationFilepath, "Agent tag registry copy"));
			if (persisted->getUuid() == source.getUuid()
				|| !persisted->hasEquivalentDefinitions(source))
			{
				throw SerializationException(
					"The persisted Agent tag registry copy is not an independent equivalent registry");
			}
			registerCreatedRegistry(destinationFilepath, persisted);
			return persisted;
		}
		catch (...)
		{
			std::error_code ignored;
			std::filesystem::remove(stagingPath, ignored);
			if (destinationInstalled)
				std::filesystem::remove(destinationFilepath, ignored);
			throw;
		}
	}

	std::shared_ptr<AgentTagRegistry> selectAndAttachAgentTagRegistry(
		Building& building, std::filesystem::path const& buildingFilepath,
		std::filesystem::path const& registryFilepath)
	{
		if (building.getAgentTagAssignmentCount() != 0)
		{
			throw SerializationException(
				"Cannot switch Agent tag registries while Agent tag assignments exist; use the confirmed destructive action to clear assignments and samples first");
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

	std::shared_ptr<AgentTagRegistry> selectAndAttachAgentTagRegistryClearingAssignments(
		Building& building, std::filesystem::path const& buildingFilepath,
		std::filesystem::path const& registryFilepath)
	{
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
		building.attachAgentTagRegistryAndClearAssignments(
			canonicalRegistry.filename().string(), registry);
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
		try
		{
			building.resolveAgentTagRegistry(registry);
		}
		catch (...)
		{
			// A registry first encountered by a refused Building open must not
			// remain even as an expired manager entry. Existing shared registries
			// retain their other owners and are therefore unaffected.
			registry.reset();
			discardUnreferencedRegistries();
			throw;
		}
		return registry;
	}

	bool reloadAgentTagRegistryDocument(
		std::shared_ptr<AgentTagRegistry> const& registry,
		std::filesystem::path const& registryFilepath,
		std::string* diagnostic)
	{
		if (diagnostic) diagnostic->clear();
		auto refuse = [diagnostic](std::string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (!registry) return refuse("There is no Agent tag registry to reload");
		if (registry->isModified())
		{
			return refuse(
				"The Agent tag registry has unsaved changes; save or discard them before reloading");
		}

		try
		{
			requireAgentTagRegistryFilename(registryFilepath);
			auto const canonicalPath = requireCanonicalRegularFile(
				registryFilepath, "Agent tag registry");
			requireAgentTagRegistryFilename(canonicalPath);

			discardUnreferencedRegistries();
			auto managed = std::find_if(gLoadedAgentTagRegistries.begin(),
				gLoadedAgentTagRegistries.end(), [&registry](auto const& entry)
				{
					return entry.registry == registry;
				});
			if (managed != gLoadedAgentTagRegistries.end()
				&& !pathsReferToSameFile(managed->canonicalPath, canonicalPath))
			{
				return refuse(std::format(
					"Agent tag registry is loaded from {}, not {}",
					managed->canonicalPath.string(), canonicalPath.string()));
			}

			auto replacement = readRegistry(canonicalPath);
			requireExpectedUuid(*replacement, registry->getUuid());
			std::string reloadDiagnostic;
			if (!registry->replaceDefinitionsFrom(
				std::move(*replacement), &reloadDiagnostic))
				return refuse(std::move(reloadDiagnostic));

			if (managed == gLoadedAgentTagRegistries.end())
				gLoadedAgentTagRegistries.push_back({ canonicalPath, registry });
			return true;
		}
		catch (std::exception const& error)
		{
			return refuse(std::format(
				"Could not reload Agent tag registry: {}", error.what()));
		}
	}

	bool unloadAgentTagRegistryDocumentIfUnused(
		std::shared_ptr<AgentTagRegistry> const& registry, bool discardDirty)
	{
		if (!registry || registry->hasLoadedBuildings()) return false;
		if (registry->isModified() && !discardDirty) return false;
		std::erase_if(gLoadedAgentTagRegistries,
			[&registry](LoadedAgentTagRegistry const& entry)
			{
				return entry.registry == registry;
			});
		return true;
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
		// A behaviour-registry refusal propagates without replacing the caller's
		// state; the temporary Building unregisters from every shared registry
		// in its destructor, so no dependent document keeps a stale pointer.
		loadAndAttachAgentBehaviourRegistry(*loaded, canonicalBuilding);
		return loaded;
	}
}
