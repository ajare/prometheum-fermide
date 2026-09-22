#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace core
{
	class AgentBehaviourRegistry;
	class Building;

	// An Agent behaviour registry package is a directory whose name ends with
	// .behaviours and whose manifest is the fixed file behaviours.yaml inside
	// it. The directory also holds the package's managed Lua source modules.
	// The default adjacent package for `/project/station.yaml` is the
	// directory `/project/station.behaviours/`.
	std::filesystem::path defaultAgentBehaviourRegistryPackagePath(
		std::filesystem::path const& buildingFilepath);
	std::filesystem::path agentBehaviourRegistryManifestPath(
		std::filesystem::path const& packageDirectory);

	// Creates a new empty package without overwriting an existing directory,
	// writes its manifest atomically, and only then attaches the package name
	// and UUID to the Building.
	std::shared_ptr<AgentBehaviourRegistry> createAndAttachAgentBehaviourRegistry(
		Building& building, std::filesystem::path const& buildingFilepath);

	// Selects an existing package. Both documents must resolve to a directory
	// and regular manifest in the same canonical directory, and only the
	// package directory name is stored in the Building.
	std::shared_ptr<AgentBehaviourRegistry> selectAndAttachAgentBehaviourRegistry(
		Building& building, std::filesystem::path const& buildingFilepath,
		std::filesystem::path const& packageDirectory);

	// Resolves a persisted Building reference beside the Building document,
	// verifies the registry UUID, and attaches it. Registries are shared by
	// canonical package identity. Returns null when the Building has no
	// reference.
	std::shared_ptr<AgentBehaviourRegistry> loadAndAttachAgentBehaviourRegistry(
		Building& building, std::filesystem::path const& buildingFilepath);

	// Explicitly reloads a clean shared registry package from disk. The
	// replacement manifest and every managed source module are validated
	// before definitions change; all dependent Buildings must already be
	// paused.
	bool reloadAgentBehaviourRegistryDocument(
		std::shared_ptr<AgentBehaviourRegistry> const& registry,
		std::filesystem::path const& packageDirectory,
		std::string* diagnostic = nullptr);

	// Removes a manager-owned registry only after its final Building detaches.
	// Dirty registries stay loaded unless discardDirty is the user's explicit
	// discard action.
	bool unloadAgentBehaviourRegistryDocumentIfUnused(
		std::shared_ptr<AgentBehaviourRegistry> const& registry,
		bool discardDirty = false);
}
