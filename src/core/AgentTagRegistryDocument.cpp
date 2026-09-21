#include "core/AgentTagRegistryDocument.h"

#include <format>
#include <system_error>

#include "core/AgentTagRegistry.h"
#include "core/Building.h"
#include "core/SerializationException.h"

namespace core
{
	namespace
	{
		std::filesystem::path requireSavedBuildingPath(
			std::filesystem::path const& buildingFilepath)
		{
			if (buildingFilepath.empty())
			{
				throw SerializationException(
					"Save the Building before creating or loading an Agent tag registry");
			}
			std::error_code error;
			auto absolute = std::filesystem::absolute(buildingFilepath, error);
			if (error) absolute = buildingFilepath;
			absolute = absolute.lexically_normal();
			if (!std::filesystem::is_regular_file(absolute, error) || error)
			{
				throw SerializationException(
					"Save the Building before creating or loading an Agent tag registry");
			}
			return absolute;
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
		building.attachAgentTagRegistry(registryPath.filename().string(), registry);
		return registry;
	}

	std::shared_ptr<AgentTagRegistry> loadAndAttachAgentTagRegistry(
		Building& building, std::filesystem::path const& buildingFilepath)
	{
		if (!building.hasAgentTagRegistryReference()) return {};
		auto const savedBuilding = requireSavedBuildingPath(buildingFilepath);
		auto const registryPath = savedBuilding.parent_path()
			/ building.getAgentTagRegistryFilename();
		auto registry = AgentTagRegistry::loadFrom(registryPath.string());
		building.resolveAgentTagRegistry(registry);
		return registry;
	}
}
