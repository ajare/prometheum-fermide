#pragma once

#include <filesystem>
#include <memory>

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

	// Resolves a persisted Building reference beside the Building document,
	// verifies the registry UUID, and attaches it. Registries are shared by
	// canonical file identity. Returns null when the Building has no reference.
	std::shared_ptr<AgentTagRegistry> loadAndAttachAgentTagRegistry(
		Building& building, std::filesystem::path const& buildingFilepath);

	// Loads a complete Building document and its optional registry into temporary
	// state. No caller-owned Building is changed when either document is refused.
	std::shared_ptr<Building> loadBuildingDocument(
		std::filesystem::path const& buildingFilepath);
}
