#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace core
{
	class AgentTagRegistry;
	class Building;

	// The default adjacent filename used by the Tags panel. For
	// `/project/station.yaml` this is `/project/station.tags.yaml`.
	std::filesystem::path defaultAgentTagRegistryPath(
		std::filesystem::path const& buildingFilepath);

	// Creates a new empty registry without overwriting an existing file, writes
	// it atomically, and only then attaches its basename and UUID to the Building.
	std::shared_ptr<AgentTagRegistry> createAndAttachAgentTagRegistry(
		Building& building, std::filesystem::path const& buildingFilepath);

	// Selects an existing registry. Both documents must resolve to regular files
	// in the same canonical directory, and only the registry basename is stored.
	std::shared_ptr<AgentTagRegistry> selectAndAttachAgentTagRegistry(
		Building& building, std::filesystem::path const& buildingFilepath,
		std::filesystem::path const& registryFilepath);

	// The explicit destructive switch validates and loads the replacement before
	// atomically clearing every assignment/sample and changing the reference.
	std::shared_ptr<AgentTagRegistry> selectAndAttachAgentTagRegistryClearingAssignments(
		Building& building, std::filesystem::path const& buildingFilepath,
		std::filesystem::path const& registryFilepath);

	// Resolves a persisted Building reference beside the Building document,
	// verifies the registry UUID, and attaches it. Registries are shared by
	// canonical file identity. Returns null when the Building has no reference.
	std::shared_ptr<AgentTagRegistry> loadAndAttachAgentTagRegistry(
		Building& building, std::filesystem::path const& buildingFilepath);

	// Explicitly reloads a clean shared registry from disk. The replacement and
	// every loaded Agent are validated before definitions or samples change; all
	// dependent Buildings must already be paused.
	bool reloadAgentTagRegistryDocument(
		std::shared_ptr<AgentTagRegistry> const& registry,
		std::filesystem::path const& registryFilepath,
		std::string* diagnostic = nullptr);

	// Removes a manager-owned registry only after its final Building detaches.
	// Dirty registries stay loaded unless discardDirty is the user's explicit
	// discard action.
	bool unloadAgentTagRegistryDocumentIfUnused(
		std::shared_ptr<AgentTagRegistry> const& registry,
		bool discardDirty = false);

	// Loads a complete Building document and its optional registry into temporary
	// state. No caller-owned Building is changed when either document is refused.
	std::shared_ptr<Building> loadBuildingDocument(
		std::filesystem::path const& buildingFilepath);
}
