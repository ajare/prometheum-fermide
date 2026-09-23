#include "core/AgentBehaviourRegistryDocument.h"

#include <algorithm>
#include <format>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
#include "core/Building.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"

namespace core
{
	namespace
	{
		char const* const ManifestFilename{ "behaviours.yaml" };

		struct LoadedAgentBehaviourRegistry
		{
			std::filesystem::path canonicalPackageDirectory;
			std::shared_ptr<AgentBehaviourRegistry> registry;
		};

		// The manager keeps dirty unreferenced packages alive so detaching one
		// Building cannot silently discard unsaved shared work. Clean packages
		// are removed as soon as no loaded Building references them.
		std::vector<LoadedAgentBehaviourRegistry> gLoadedAgentBehaviourRegistries;

		void discardUnreferencedRegistries()
		{
			std::erase_if(gLoadedAgentBehaviourRegistries,
				[](LoadedAgentBehaviourRegistry const& entry)
				{
					return !entry.registry || (!entry.registry->hasLoadedBuildings()
						&& !entry.registry->isModified());
				});
		}

		bool pathsReferToSameDirectory(std::filesystem::path const& left,
			std::filesystem::path const& right)
		{
			// Registry identity is its canonical absolute package directory. A
			// hard link or bind alias under a second canonical path is therefore a
			// second package and is caught by the duplicate-UUID rule below rather
			// than silently becoming an alias.
			return left == right;
		}

		std::filesystem::path requireCanonicalPackageDirectory(
			std::filesystem::path const& packageDirectory)
		{
			if (packageDirectory.empty())
			{
				throw SerializationException("Agent behaviour registry package path is empty");
			}
			auto const filename = packageDirectory.filename().string();
			if (filename.empty() || !filename.ends_with(".behaviours"))
			{
				throw SerializationException(
					"An Agent behaviour registry package directory must end with .behaviours");
			}

			std::error_code error;
			auto const canonical = std::filesystem::canonical(packageDirectory, error);
			if (error || !std::filesystem::is_directory(canonical, error) || error)
			{
				throw SerializationException(std::format(
					"Agent behaviour registry package is missing or is not a directory: {}",
					packageDirectory.string()));
			}
			if (!canonical.filename().string().ends_with(".behaviours"))
			{
				throw SerializationException(std::format(
					"Agent behaviour registry package is missing or is not a directory: {}",
					packageDirectory.string()));
			}
			return canonical;
		}

		std::filesystem::path requireManifest(
			std::filesystem::path const& canonicalPackageDirectory)
		{
			auto const manifestPath = canonicalPackageDirectory / ManifestFilename;
			std::error_code error;
			if (!std::filesystem::is_regular_file(manifestPath, error) || error)
			{
				throw SerializationException(std::format(
					"Agent behaviour registry manifest is missing or is not a regular file: {}",
					manifestPath.string()));
			}
			auto const canonical = std::filesystem::canonical(manifestPath);
			if (canonical.parent_path() != canonicalPackageDirectory)
				throw SerializationException("Agent behaviour manifest escapes its package directory");
			return canonical;
		}

		std::filesystem::path requireSavedBuildingPath(
			std::filesystem::path const& buildingFilepath)
		{
			if (buildingFilepath.empty())
			{
				throw SerializationException(
					"Save the Building before creating, selecting, or loading an Agent behaviour registry");
			}
			std::error_code error;
			auto const canonical = std::filesystem::canonical(buildingFilepath, error);
			if (error || !std::filesystem::is_regular_file(canonical, error) || error)
			{
				throw SerializationException(
					"Save the Building before creating, selecting, or loading an Agent behaviour registry");
			}
			return canonical;
		}

		std::shared_ptr<AgentBehaviourRegistry> readRegistry(
			std::filesystem::path const& canonicalPackageDirectory)
		{
			try
			{
				return AgentBehaviourRegistry::loadFrom(requireManifest(
					canonicalPackageDirectory).string());
			}
			catch (std::exception const& error)
			{
				throw SerializationException(std::format(
					"Could not load Agent behaviour registry package {}: {}",
					canonicalPackageDirectory.string(), error.what()));
			}
		}

		void requireExpectedUuid(AgentBehaviourRegistry const& registry,
			std::optional<std::string> const& expectedUuid)
		{
			if (expectedUuid && registry.getUuid() != *expectedUuid)
			{
				throw SerializationException(std::format(
					"Agent behaviour registry UUID mismatch: Building expects {}, file contains {}",
					*expectedUuid, registry.getUuid()));
			}
		}

		std::shared_ptr<AgentBehaviourRegistry> loadSharedRegistry(
			std::filesystem::path const& packageDirectory,
			std::optional<std::string> const& expectedUuid = std::nullopt)
		{
			auto const canonicalDirectory = requireCanonicalPackageDirectory(packageDirectory);

			// Validate the package on every attachment, even when its loaded
			// instance is already shared. This catches deletion, substitution,
			// malformed manifests, missing source modules, and unsupported
			// schemas without replacing any in-memory state.
			auto diskRegistry = readRegistry(canonicalDirectory);
			requireExpectedUuid(*diskRegistry, expectedUuid);

			discardUnreferencedRegistries();
			for (auto const& entry : gLoadedAgentBehaviourRegistries)
			{
				auto const& loaded = entry.registry;
				if (!loaded || !pathsReferToSameDirectory(
					entry.canonicalPackageDirectory, canonicalDirectory)) continue;
				if (loaded->getUuid() != diskRegistry->getUuid())
				{
					throw SerializationException(std::format(
						"Agent behaviour registry package at {} was substituted: loaded UUID {}, file contains {}",
						canonicalDirectory.string(), loaded->getUuid(), diskRegistry->getUuid()));
				}
				if (loaded->fileHasExternalChanges(requireManifest(canonicalDirectory).string()))
				{
					throw SerializationException(std::format(
						"Agent behaviour registry package {} changed outside the editor; reload it before attaching another Building",
						canonicalDirectory.string()));
				}
				return loaded;
			}

			for (auto const& entry : gLoadedAgentBehaviourRegistries)
			{
				auto const& loaded = entry.registry;
				if (loaded && loaded->getUuid() == diskRegistry->getUuid())
				{
					throw SerializationException(std::format(
						"Agent behaviour registry UUID {} is already loaded from another package: {} (refusing {})",
						diskRegistry->getUuid(), entry.canonicalPackageDirectory.string(),
						canonicalDirectory.string()));
				}
			}

			gLoadedAgentBehaviourRegistries.push_back({ canonicalDirectory, diskRegistry });
			return diskRegistry;
		}

		void registerCreatedRegistry(std::filesystem::path const& packageDirectory,
			std::shared_ptr<AgentBehaviourRegistry> const& registry)
		{
			auto const canonicalDirectory = requireCanonicalPackageDirectory(packageDirectory);
			discardUnreferencedRegistries();
			for (auto const& entry : gLoadedAgentBehaviourRegistries)
			{
				auto const& loaded = entry.registry;
				if (!loaded) continue;
				if (pathsReferToSameDirectory(entry.canonicalPackageDirectory, canonicalDirectory))
				{
					throw SerializationException(std::format(
						"Agent behaviour registry package is already loaded from {}",
						entry.canonicalPackageDirectory.string()));
				}
				if (loaded->getUuid() == registry->getUuid())
				{
					throw SerializationException(std::format(
						"Agent behaviour registry UUID {} is already loaded from another package: {}",
						registry->getUuid(), entry.canonicalPackageDirectory.string()));
				}
			}
			gLoadedAgentBehaviourRegistries.push_back({ canonicalDirectory, registry });
		}
	}

	std::filesystem::path defaultAgentBehaviourRegistryPackagePath(
		std::filesystem::path const& buildingFilepath)
	{
		if (buildingFilepath.empty()) return {};
		auto filename = buildingFilepath.filename();
		filename.replace_extension();
		filename += ".behaviours";
		return buildingFilepath.parent_path() / filename;
	}

	std::filesystem::path agentBehaviourRegistryManifestPath(
		std::filesystem::path const& packageDirectory)
	{
		if (packageDirectory.empty()) return {};
		return packageDirectory / ManifestFilename;
	}

	std::shared_ptr<AgentBehaviourRegistry> createAndAttachAgentBehaviourRegistry(
		Building& building, std::filesystem::path const& buildingFilepath)
	{
		if (!building.isSimulationPaused())
			throw SerializationException("Pause the Building before creating an Agent behaviour registry");
		if (building.hasAgentBehaviourRegistryReference())
		{
			throw SerializationException("The Building already references an Agent behaviour registry");
		}
		auto const savedBuilding = requireSavedBuildingPath(buildingFilepath);
		auto const packageDirectory = defaultAgentBehaviourRegistryPackagePath(savedBuilding);
		std::error_code error;
		auto const status = std::filesystem::symlink_status(packageDirectory, error);
		if (!error && status.type() != std::filesystem::file_type::not_found)
		{
			throw SerializationException(std::format(
				"Agent behaviour registry package already exists: {}",
				packageDirectory.string()));
		}
		if (error && error != std::errc::no_such_file_or_directory)
		{
			throw SerializationException(std::format(
				"Could not inspect Agent behaviour registry package {}: {}",
				packageDirectory.string(), error.message()));
		}

		auto registry = AgentBehaviourRegistry::create();
		// YamlSerializer installs the completed temporary manifest with one
		// rename. Attachment happens afterwards, so a failed write cannot leave
		// the Building referring to a partial or absent package.
		error.clear();
		auto const created = std::filesystem::create_directory(packageDirectory, error);
		if (!created || error)
		{
			throw SerializationException(std::format(
				"Could not create Agent behaviour registry package {}: {}",
				packageDirectory.string(), error.message()));
		}
		try
		{
			registry->saveTo(agentBehaviourRegistryManifestPath(packageDirectory).string());
			registerCreatedRegistry(packageDirectory, registry);
		}
		catch (...)
		{
			std::error_code ignored;
			std::filesystem::remove_all(packageDirectory, ignored);
			throw;
		}
		building.attachAgentBehaviourRegistry(packageDirectory.filename().string(), registry);
		return registry;
	}

	std::shared_ptr<AgentBehaviourRegistry> selectAndAttachAgentBehaviourRegistry(
		Building& building, std::filesystem::path const& buildingFilepath,
		std::filesystem::path const& packageDirectory)
	{
		if (!building.isSimulationPaused())
			throw SerializationException("Pause the Building before selecting an Agent behaviour registry");
		auto const savedBuilding = requireSavedBuildingPath(buildingFilepath);
		auto const canonicalDirectory = requireCanonicalPackageDirectory(packageDirectory);
		if (canonicalDirectory.parent_path() != savedBuilding.parent_path())
		{
			throw SerializationException(
				"An Agent behaviour registry package must be in the same directory as its Building");
		}

		auto registry = loadSharedRegistry(canonicalDirectory);
		building.attachAgentBehaviourRegistry(canonicalDirectory.filename().string(), registry);
		return registry;
	}

	std::shared_ptr<AgentBehaviourRegistry> loadAndAttachAgentBehaviourRegistry(
		Building& building, std::filesystem::path const& buildingFilepath)
	{
		if (!building.hasAgentBehaviourRegistryReference()) return {};
		auto const savedBuilding = requireSavedBuildingPath(buildingFilepath);
		auto const packageDirectory = savedBuilding.parent_path()
			/ building.getAgentBehaviourRegistryPackageName();
		auto const canonical = requireCanonicalPackageDirectory(packageDirectory);
		if (canonical.parent_path() != savedBuilding.parent_path())
			throw SerializationException("Agent behaviour package must remain beside its Building");
		auto registry = loadSharedRegistry(canonical,
			building.getExpectedAgentBehaviourRegistryUuid());
		try
		{
			building.resolveAgentBehaviourRegistry(registry);
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

	bool previewAgentBehaviourRegistrySchemaMigration(
		std::shared_ptr<AgentBehaviourRegistry> const& registry,
		std::filesystem::path const& packageDirectory,
		AgentBehaviourSchemaMigrationPreview& preview,
		std::string* diagnostic)
	{
		preview = {};
		if (diagnostic) diagnostic->clear();
		if (!registry)
		{
			if (diagnostic) *diagnostic = "There is no Agent behaviour registry to preview";
			return false;
		}
		std::string pausedDiagnostic;
		if (!registry->definitionEditsAreAllowed(&pausedDiagnostic))
		{
			if (diagnostic) *diagnostic = std::move(pausedDiagnostic);
			return false;
		}
		try
		{
			auto const canonicalDirectory = requireCanonicalPackageDirectory(
				packageDirectory);
			auto replacement = readRegistry(canonicalDirectory);
			requireExpectedUuid(*replacement, registry->getUuid());
			return registry->previewDefinitionsFrom(*replacement, preview, diagnostic);
		}
		catch (std::exception const& error)
		{
			if (diagnostic) *diagnostic = std::format(
				"Could not preview Agent behaviour schema migration: {}", error.what());
			return false;
		}
	}

	bool reloadAgentBehaviourRegistryDocument(
		std::shared_ptr<AgentBehaviourRegistry> const& registry,
		std::filesystem::path const& packageDirectory,
		std::string* diagnostic,
		std::vector<AgentBehaviourReloadDiagnostic>* reloadDiagnostics)
	{
		if (diagnostic) diagnostic->clear();
		if (reloadDiagnostics) reloadDiagnostics->clear();
		auto refuse = [diagnostic, reloadDiagnostics](std::string message)
		{
			if (diagnostic) *diagnostic = message;
			if (reloadDiagnostics) reloadDiagnostics->push_back({
				AgentBehaviourReloadDiagnosticScope::Package, {}, {}, {}, {}, {}, {},
				std::move(message), {} });
			return false;
		};
		if (!registry) return refuse("There is no Agent behaviour registry to reload");
		if (registry->isModified())
		{
			return refuse(
				"The Agent behaviour registry has unsaved changes; save or discard them before reloading");
		}

		std::string pausedDiagnostic;
		if (!registry->definitionEditsAreAllowed(&pausedDiagnostic))
			return refuse(std::move(pausedDiagnostic));

		try
		{
			auto const canonicalDirectory = requireCanonicalPackageDirectory(packageDirectory);
			requireManifest(canonicalDirectory);

			discardUnreferencedRegistries();
			auto managed = std::find_if(gLoadedAgentBehaviourRegistries.begin(),
				gLoadedAgentBehaviourRegistries.end(), [&registry](auto const& entry)
				{
					return entry.registry == registry;
				});
			if (managed != gLoadedAgentBehaviourRegistries.end()
				&& !pathsReferToSameDirectory(managed->canonicalPackageDirectory,
					canonicalDirectory))
			{
				return refuse(std::format(
					"Agent behaviour registry is loaded from {}, not {}",
					managed->canonicalPackageDirectory.string(), canonicalDirectory.string()));
			}

			auto replacement = readRegistry(canonicalDirectory);
			requireExpectedUuid(*replacement, registry->getUuid());
			std::string reloadDiagnostic;
			if (!registry->replaceDefinitionsFrom(
				std::move(*replacement), &reloadDiagnostic, reloadDiagnostics))
			{
				if (diagnostic) *diagnostic = std::move(reloadDiagnostic);
				return false;
			}

			if (managed == gLoadedAgentBehaviourRegistries.end())
				gLoadedAgentBehaviourRegistries.push_back({ canonicalDirectory, registry });
			return true;
		}
		catch (std::exception const& error)
		{
			return refuse(std::format(
				"Could not reload Agent behaviour registry: {}", error.what()));
		}
	}

	bool migrateAgentBehaviourRegistryDocument(
		std::shared_ptr<AgentBehaviourRegistry> const& registry,
		std::filesystem::path const& packageDirectory,
		std::vector<AgentBehaviourConfigurationMigration> const& migrations,
		std::string* diagnostic,
		std::vector<AgentBehaviourReloadDiagnostic>* reloadDiagnostics)
	{
		if (diagnostic) diagnostic->clear();
		if (reloadDiagnostics) reloadDiagnostics->clear();
		auto refuse = [diagnostic, reloadDiagnostics](std::string message)
		{
			if (diagnostic) *diagnostic = message;
			if (reloadDiagnostics) reloadDiagnostics->push_back({
				AgentBehaviourReloadDiagnosticScope::Package, {}, {}, {}, {}, {}, {},
				std::move(message), {} });
			return false;
		};
		if (!registry) return refuse("There is no Agent behaviour registry to migrate");
		if (registry->isModified())
			return refuse("The Agent behaviour registry has unsaved changes; save or discard them before migrating");
		std::string pausedDiagnostic;
		if (!registry->definitionEditsAreAllowed(&pausedDiagnostic))
			return refuse(std::move(pausedDiagnostic));
		try
		{
			auto const canonicalDirectory = requireCanonicalPackageDirectory(
				packageDirectory);
			auto replacement = readRegistry(canonicalDirectory);
			requireExpectedUuid(*replacement, registry->getUuid());
			std::string migrationDiagnostic;
			if (!registry->replaceDefinitionsFrom(std::move(*replacement),
				&migrationDiagnostic, reloadDiagnostics, migrations))
			{
				if (diagnostic) *diagnostic = std::move(migrationDiagnostic);
				return false;
			}
			return true;
		}
		catch (std::exception const& error)
		{
			return refuse(std::format(
				"Could not migrate Agent behaviour registry: {}", error.what()));
		}
	}

	bool unloadAgentBehaviourRegistryDocumentIfUnused(
		std::shared_ptr<AgentBehaviourRegistry> const& registry, bool discardDirty)
	{
		if (!registry || registry->hasLoadedBuildings()) return false;
		if (registry->isModified() && !discardDirty) return false;
		std::erase_if(gLoadedAgentBehaviourRegistries,
			[&registry](LoadedAgentBehaviourRegistry const& entry)
			{
				return entry.registry == registry;
			});
		return true;
	}
}
