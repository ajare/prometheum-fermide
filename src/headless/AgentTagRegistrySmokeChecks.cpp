// Empty external Agent tag registry document workflow checks for #128.

#include "TagsPanel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Building.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path;

		TemporaryDirectory()
		{
			path = std::filesystem::temp_directory_path()
				/ ("promethium-fermide-tags-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}

		~TemporaryDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	std::string readText(std::filesystem::path const& path)
	{
		std::ifstream input(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	std::string serializeBuilding(core::Building const& building)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		building.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::Building> loadBuilding(std::filesystem::path const& path)
	{
		auto loaded = std::make_shared<core::Building>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromFile(path.string());
		reader->deserialize();
		core::SerializationWorkData workData;
		require(loaded->deserialize(*reader, workData), "The Building did not reload");
		return loaded;
	}

	void savedBuildingCreatesAndReopensAdjacentRegistry()
	{
		TemporaryDirectory temporary;
		auto building = std::make_shared<core::Building>("Station", 4, 2);
		auto const buildingPath = temporary.path / "station.yaml";

		std::string diagnostic;
		require(!canCreateAgentTagRegistry(building, "", &diagnostic),
			"The Tags panel enabled registry creation before the Building was saved");
		require(diagnostic.find("Save") != std::string::npos,
			"The unsaved-registry diagnostic did not explain the saved-location requirement");

		building->saveTo(buildingPath.string());
		require(canCreateAgentTagRegistry(building, buildingPath.string(), &diagnostic),
			"The Tags panel did not enable registry creation for a saved Building");
		auto registry = core::createAndAttachAgentTagRegistry(*building, buildingPath);
		auto const registryPath = temporary.path / "station.tags.yaml";
		require(std::filesystem::is_regular_file(registryPath),
			"The adjacent .tags.yaml registry was not created");
		require(building->hasAgentTagRegistryReference()
			&& building->hasAttachedAgentTagRegistry(),
			"The new registry was not attached to the Building");
		require(building->getAgentTagRegistryFilename() == "station.tags.yaml",
			"The Building did not retain a basename-only registry reference");
		require(building->getExpectedAgentTagRegistryUuid() == registry->getUuid()
			&& core::AgentTagRegistry::uuidIsValid(registry->getUuid()),
			"The Building did not retain the registry's stable UUID");
		require(registry->getNextAgentTagId() == 1
			&& registry->getNextPropertyRevision() == 1,
			"A new registry did not start both non-reused allocators at one");

		auto const registryYaml = readText(registryPath);
		require(registryYaml.find("version: 1") != std::string::npos
			&& registryYaml.find("uuid: " + registry->getUuid()) != std::string::npos
			&& registryYaml.find("nextAgentTagId: 1") != std::string::npos
			&& registryYaml.find("nextPropertyRevision: 1") != std::string::npos
			&& registryYaml.find("tags:") != std::string::npos,
			"The empty registry omitted its schema, UUID, allocators, or tag collection");

		// Persist the attachment, then model closing both documents and reopening
		// through the same core workflow used by the GUI.
		building->saveTo(buildingPath.string());
		auto const buildingYaml = readText(buildingPath);
		require(buildingYaml.find("version: 10") != std::string::npos
			&& buildingYaml.find("filename: station.tags.yaml") != std::string::npos
			&& buildingYaml.find("expectedUuid: " + registry->getUuid()) != std::string::npos,
			"The Building did not persist its version-10 registry reference");

		auto reopened = loadBuilding(buildingPath);
		require(reopened->hasAgentTagRegistryReference()
			&& !reopened->hasAttachedAgentTagRegistry(),
			"Building deserialization did not retain an unresolved registry reference");
		auto reopenedRegistry = core::loadAndAttachAgentTagRegistry(*reopened, buildingPath);
		require(reopened->hasAttachedAgentTagRegistry()
			&& reopenedRegistry->getUuid() == registry->getUuid(),
			"The referenced empty registry did not survive close and reopen");

		// Replacing the adjacent file must not silently reinterpret the reference.
		auto replacement = core::AgentTagRegistry::create();
		replacement->saveTo(registryPath.string());
		auto substituted = loadBuilding(buildingPath);
		bool mismatchRefused{ false };
		try
		{
			(void)core::loadAndAttachAgentTagRegistry(*substituted, buildingPath);
		}
		catch (std::exception const& error)
		{
			mismatchRefused = std::string(error.what()).find("UUID mismatch")
				!= std::string::npos;
		}
		require(mismatchRefused && !substituted->hasAttachedAgentTagRegistry(),
			"A substituted registry with a different UUID was attached");
	}

	void olderBuildingWithoutReferenceStillLoads()
	{
		core::Building source("Legacy", 4, 2);
		auto yaml = serializeBuilding(source);
		auto const version = yaml.find("version: 10");
		require(version != std::string::npos, "The current Building schema was not version 10");
		yaml.replace(version, std::string("version: 10").size(), "version: 9");

		auto loaded = std::make_shared<core::Building>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData workData;
		require(loaded->deserialize(*reader, workData), "A version-9 Building did not load");
		require(!loaded->hasAgentTagRegistryReference()
			&& !loaded->hasAttachedAgentTagRegistry(),
			"An older Building invented an Agent tag registry");
	}

	void failedAtomicCreationLeavesNoReferenceOrFile()
	{
		TemporaryDirectory temporary;
		core::Building building("Atomic", 4, 2);
		auto const buildingPath = temporary.path / "atomic.yaml";
		building.saveTo(buildingPath.string());
		auto const registryPath = core::defaultAgentTagRegistryPath(buildingPath);

		core::YamlSerializer::setWriteFailureAfterBytesForTesting(1);
		bool refused{ false };
		try
		{
			(void)core::createAndAttachAgentTagRegistry(building, buildingPath);
		}
		catch (std::exception const&)
		{
			refused = true;
		}
		core::YamlSerializer::setWriteFailureAfterBytesForTesting(0);
		require(refused, "The injected registry write failure was not reported");
		require(!std::filesystem::exists(registryPath),
			"A failed registry creation left a partial destination file");
		require(!building.hasAgentTagRegistryReference(),
			"A failed registry creation attached a nonexistent registry");
	}
}

void runAgentTagRegistrySmokeChecks()
{
	savedBuildingCreatesAndReopensAdjacentRegistry();
	olderBuildingWithoutReferenceStillLoads();
	failedAtomicCreationLeavesNoReferenceOrFile();
}
