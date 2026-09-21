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

	// Resolves a persisted Building reference beside the Building document,
	// verifies the registry UUID, and attaches it. Returns null when the Building
	// has no registry reference.
	std::shared_ptr<AgentTagRegistry> loadAndAttachAgentTagRegistry(
		Building& building, std::filesystem::path const& buildingFilepath);
}
