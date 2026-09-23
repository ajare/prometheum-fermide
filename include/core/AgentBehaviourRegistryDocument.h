#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace core
{
	class AgentBehaviourRegistry;
	class World;
	struct AgentBehaviourReloadDiagnostic;
	struct AgentBehaviourSchemaMigrationPreview;
	struct AgentBehaviourConfigurationMigration;

	// An Agent behaviour registry package is a directory whose name ends with
	// .behaviours and whose manifest is the fixed file behaviours.yaml inside
	// it. The directory also holds the package's managed Lua source modules.
	// The default adjacent package for `/project/station.world.yaml` is the
	// directory `/project/station.behaviours/`.
	std::filesystem::path defaultAgentBehaviourRegistryPackagePath(
		std::filesystem::path const& worldFilepath);
	std::filesystem::path agentBehaviourRegistryManifestPath(
		std::filesystem::path const& packageDirectory);

	// Creates a new empty package without overwriting an existing directory,
	// writes its manifest atomically, and only then attaches the package name
	// and UUID to the World.
	std::shared_ptr<AgentBehaviourRegistry> createAndAttachAgentBehaviourRegistry(
		World& world, std::filesystem::path const& worldFilepath);

	// Installs an independent no-clobber package copy. Only manifest-declared
	// behaviour and helper Lua modules are copied; failure removes the whole
	// destination package.
	std::shared_ptr<AgentBehaviourRegistry> copyAgentBehaviourRegistryDocument(
		AgentBehaviourRegistry const& source,
		std::filesystem::path const& sourcePackageDirectory,
		std::filesystem::path const& destinationPackageDirectory);

	// Selects an existing package. Both documents must resolve to a directory
	// and regular manifest in the same canonical directory, and only the
	// package directory name is stored in the World.
	std::shared_ptr<AgentBehaviourRegistry> selectAndAttachAgentBehaviourRegistry(
		World& world, std::filesystem::path const& worldFilepath,
		std::filesystem::path const& packageDirectory);

	// Explicit destructive replacement. The complete candidate package is read
	// and validated before the World atomically clears every assignment and
	// configuration or changes its persisted reference.
	std::shared_ptr<AgentBehaviourRegistry>
	selectAndAttachAgentBehaviourRegistryClearingAssignments(
		World& world, std::filesystem::path const& worldFilepath,
		std::filesystem::path const& packageDirectory);

	// Resolves a persisted World reference beside the World document,
	// verifies the registry UUID, and attaches it. Registries are shared by
	// canonical package identity. Missing, substituted, unsupported, or invalid
	// packages become a recoverable dependency diagnostic and return null while
	// structural and unresolved authored World data remain loaded.
	std::shared_ptr<AgentBehaviourRegistry> loadAndAttachAgentBehaviourRegistry(
		World& world, std::filesystem::path const& worldFilepath);

	// Explicitly reloads a clean shared registry package from disk. The
	// replacement manifest and every managed source module are validated
	// before definitions change; all dependent Worlds must already be
	// paused.
	bool previewAgentBehaviourRegistrySchemaMigration(
		std::shared_ptr<AgentBehaviourRegistry> const& registry,
		std::filesystem::path const& packageDirectory,
		AgentBehaviourSchemaMigrationPreview& preview,
		std::string* diagnostic = nullptr);

	bool reloadAgentBehaviourRegistryDocument(
		std::shared_ptr<AgentBehaviourRegistry> const& registry,
		std::filesystem::path const& packageDirectory,
		std::string* diagnostic = nullptr,
		std::vector<AgentBehaviourReloadDiagnostic>* reloadDiagnostics = nullptr);

	// Explicitly adopts an incompatible candidate only when every affected
	// configuration has a complete validated C++ replacement. Preview and commit
	// reread the same managed package and the commit remains all-or-nothing.
	bool migrateAgentBehaviourRegistryDocument(
		std::shared_ptr<AgentBehaviourRegistry> const& registry,
		std::filesystem::path const& packageDirectory,
		std::vector<AgentBehaviourConfigurationMigration> const& migrations,
		std::string* diagnostic = nullptr,
		std::vector<AgentBehaviourReloadDiagnostic>* reloadDiagnostics = nullptr);

	// Removes a manager-owned registry only after its final World detaches.
	// Dirty registries stay loaded unless discardDirty is the user's explicit
	// discard action.
	bool unloadAgentBehaviourRegistryDocumentIfUnused(
		std::shared_ptr<AgentBehaviourRegistry> const& registry,
		bool discardDirty = false);
}
