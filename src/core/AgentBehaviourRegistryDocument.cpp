#include "core/AgentBehaviourRegistryDocument.h"

#include <algorithm>
#include <format>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
#include "core/World.h"
#include "core/WorldDocument.h"
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
		// World cannot silently discard unsaved shared work. Clean packages
		// are removed as soon as no loaded World references them.
		std::vector<LoadedAgentBehaviourRegistry> gLoadedAgentBehaviourRegistries;

		void discardUnreferencedRegistries()
		{
			std::erase_if(gLoadedAgentBehaviourRegistries,
				[](LoadedAgentBehaviourRegistry const& entry)
				{
					return !entry.registry || (!entry.registry->hasLoadedWorlds()
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

		std::filesystem::path requireSavedWorldPath(
			std::filesystem::path const& worldFilepath)
		{
			if (worldFilepath.empty())
			{
				throw SerializationException(
					"Save the World before creating, selecting, or loading an Agent behaviour registry");
			}
			requireWorldDocumentPath(worldFilepath);
			std::error_code error;
			auto const canonical = std::filesystem::canonical(worldFilepath, error);
			if (error || !std::filesystem::is_regular_file(canonical, error) || error)
			{
				throw SerializationException(
					"Save the World before creating, selecting, or loading an Agent behaviour registry");
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
					"Agent behaviour registry UUID mismatch: World expects {}, file contains {}",
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
						"Agent behaviour registry package {} changed outside the editor; reload it before attaching another World",
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
		std::filesystem::path const& worldFilepath)
	{
		if (worldFilepath.empty()) return {};
		auto path = worldDocumentBasePath(worldFilepath);
		path += ".behaviours";
		return path;
	}

	std::filesystem::path agentBehaviourRegistryManifestPath(
		std::filesystem::path const& packageDirectory)
	{
		if (packageDirectory.empty()) return {};
		return packageDirectory / ManifestFilename;
	}

	std::shared_ptr<AgentBehaviourRegistry> createAndAttachAgentBehaviourRegistry(
		World& world, std::filesystem::path const& worldFilepath)
	{
		if (!world.isSimulationPaused())
			throw SerializationException("Pause the World before creating an Agent behaviour registry");
		if (world.hasAgentBehaviourRegistryReference())
		{
			throw SerializationException("The World already references an Agent behaviour registry");
		}
		auto const savedWorld = requireSavedWorldPath(worldFilepath);
		auto const packageDirectory = defaultAgentBehaviourRegistryPackagePath(savedWorld);
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
		// the World referring to a partial or absent package.
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
		world.attachAgentBehaviourRegistry(packageDirectory.filename().string(), registry);
		return registry;
	}

	std::shared_ptr<AgentBehaviourRegistry> copyAgentBehaviourRegistryDocument(
		AgentBehaviourRegistry const& source,
		std::filesystem::path const& sourcePackageDirectory,
		std::filesystem::path const& destinationPackageDirectory)
	{
		auto const canonicalSource = requireCanonicalPackageDirectory(
			sourcePackageDirectory);
		requireManifest(canonicalSource);
		if (destinationPackageDirectory.empty()
			|| !destinationPackageDirectory.filename().string().ends_with(".behaviours"))
			throw SerializationException(
				"An Agent behaviour registry copy destination must end with .behaviours");
		std::error_code error;
		auto const status = std::filesystem::symlink_status(
			destinationPackageDirectory, error);
		if (!error && status.type() != std::filesystem::file_type::not_found)
			throw SerializationException(std::format(
				"Agent behaviour registry package copy already exists: {}",
				destinationPackageDirectory.string()));
		if (error && error != std::errc::no_such_file_or_directory)
			throw SerializationException(std::format(
				"Could not inspect Agent behaviour registry package copy destination: {}",
				error.message()));

		auto copy = source.makeIndependentCopy();
		if (!std::filesystem::create_directory(destinationPackageDirectory, error)
			|| error)
			throw SerializationException(std::format(
				"Could not create Agent behaviour registry package copy {}: {}",
				destinationPackageDirectory.string(), error.message()));
		try
		{
			std::set<std::string> modules;
			for (auto const& name : source.getHelperModuleNames())
				modules.insert(source.lookupHelperModule(name)->getSourceModulePath());
			for (auto const id : source.getBehaviourIds())
				modules.insert(source.lookupAgentBehaviour(id)->getSourceModulePath());
			for (auto const& module : modules)
			{
				auto const sourcePath = std::filesystem::weakly_canonical(
					canonicalSource / module, error);
				if (error || !std::filesystem::is_regular_file(sourcePath, error) || error
					|| sourcePath.lexically_relative(canonicalSource).empty()
					|| *sourcePath.lexically_relative(canonicalSource).begin() == "..")
					throw SerializationException(
						"Managed Agent behaviour module is missing or escapes its package: " + module);
				auto const destination = destinationPackageDirectory / module;
				std::filesystem::create_directories(destination.parent_path());
				if (!std::filesystem::copy_file(sourcePath, destination,
					std::filesystem::copy_options::none, error) || error)
					throw SerializationException(std::format(
						"Could not copy managed Agent behaviour module '{}': {}",
						module, error.message()));
			}
			copy->saveTo(agentBehaviourRegistryManifestPath(
				destinationPackageDirectory).string());
			registerCreatedRegistry(destinationPackageDirectory, copy);
			return copy;
		}
		catch (...)
		{
			std::error_code ignored;
			std::filesystem::remove_all(destinationPackageDirectory, ignored);
			throw;
		}
	}

	std::shared_ptr<AgentBehaviourRegistry> selectAndAttachAgentBehaviourRegistry(
		World& world, std::filesystem::path const& worldFilepath,
		std::filesystem::path const& packageDirectory)
	{
		if (!world.isSimulationPaused())
			throw SerializationException("Pause the World before selecting an Agent behaviour registry");
		auto const savedWorld = requireSavedWorldPath(worldFilepath);
		auto const canonicalDirectory = requireCanonicalPackageDirectory(packageDirectory);
		if (canonicalDirectory.parent_path() != savedWorld.parent_path())
		{
			throw SerializationException(
				"An Agent behaviour registry package must be in the same directory as its World");
		}

		auto registry = loadSharedRegistry(canonicalDirectory);
		auto const packageName = canonicalDirectory.filename().string();
		if (world.hasAgentBehaviourRegistryReference()
			&& !world.hasAttachedAgentBehaviourRegistry()
			&& world.getAgentBehaviourRegistryPackageName() == packageName
			&& world.getExpectedAgentBehaviourRegistryUuid() == registry->getUuid())
		{
			// Repairing the persisted expected dependency preserves its reference and
			// uses the open-time schema reconciliation/runtime transaction.
			world.resolveAgentBehaviourRegistry(registry);
		}
		else world.attachAgentBehaviourRegistry(packageName, registry);
		return registry;
	}

	std::shared_ptr<AgentBehaviourRegistry>
	selectAndAttachAgentBehaviourRegistryClearingAssignments(
		World& world, std::filesystem::path const& worldFilepath,
		std::filesystem::path const& packageDirectory)
	{
		if (!world.isSimulationPaused())
			throw SerializationException("Pause the World before replacing an Agent behaviour registry");
		auto const savedWorld = requireSavedWorldPath(worldFilepath);
		auto const canonicalDirectory = requireCanonicalPackageDirectory(packageDirectory);
		if (canonicalDirectory.parent_path() != savedWorld.parent_path())
		{
			throw SerializationException(
				"An Agent behaviour registry package must be in the same directory as its World");
		}

		// loadSharedRegistry parses the complete manifest, contains every source
		// path, and preflights the package before the destructive World method
		// can clear a single authored value.
		auto registry = loadSharedRegistry(canonicalDirectory);
		world.attachAgentBehaviourRegistryAndClearAssignments(
			canonicalDirectory.filename().string(), registry);
		return registry;
	}

	std::shared_ptr<AgentBehaviourRegistry> loadAndAttachAgentBehaviourRegistry(
		World& world, std::filesystem::path const& worldFilepath)
	{
		if (!world.hasAgentBehaviourRegistryReference()) return {};
		std::shared_ptr<AgentBehaviourRegistry> registry;
		try
		{
			auto const savedWorld = requireSavedWorldPath(worldFilepath);
			auto const packageDirectory = savedWorld.parent_path()
				/ world.getAgentBehaviourRegistryPackageName();
			auto const canonical = requireCanonicalPackageDirectory(packageDirectory);
			if (canonical.parent_path() != savedWorld.parent_path())
				throw SerializationException("Agent behaviour package must remain beside its World");
			registry = loadSharedRegistry(canonical,
				world.getExpectedAgentBehaviourRegistryUuid());
			world.resolveAgentBehaviourRegistry(registry);
			return registry;
		}
		catch (std::exception const& error)
		{
			// Dependency failure is recoverable World state. A registry first
			// encountered by this attempt is released unless another loaded World
			// already owns it; authored structure, reference, and assignments survive.
			registry.reset();
			discardUnreferencedRegistries();
			world.markAgentBehaviourRegistryUnavailable(error.what());
			return {};
		}
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
		if (!registry || registry->hasLoadedWorlds()) return false;
		if (registry->isModified() && !discardDirty) return false;
		std::erase_if(gLoadedAgentBehaviourRegistries,
			[&registry](LoadedAgentBehaviourRegistry const& entry)
			{
				return entry.registry == registry;
			});
		return true;
	}
}
