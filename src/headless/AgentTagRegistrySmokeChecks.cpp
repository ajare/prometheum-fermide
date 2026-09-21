// External Agent tag registry document workflow checks for #128 and #129.

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

	void writeText(std::filesystem::path const& path, std::string const& text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output) throw std::runtime_error("Could not write registry test fixture");
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

	void selectionEnforcesBasenameExtensionAndDirectory()
	{
		TemporaryDirectory temporary;
		auto const project = temporary.path / "project";
		auto const otherProject = temporary.path / "other";
		std::filesystem::create_directories(project);
		std::filesystem::create_directories(otherProject);

		auto registry = core::AgentTagRegistry::create();
		auto const registryPath = project / "shared.tags.yaml";
		registry->saveTo(registryPath.string());

		auto building = std::make_shared<core::Building>("Selection", 4, 2);
		auto const buildingPath = project / "selection.yaml";
		building->saveTo(buildingPath.string());
		std::string diagnostic;
		require(canSelectAgentTagRegistry(building, buildingPath.string(), &diagnostic),
			"The Tags panel did not enable selection for a saved Building");
		auto selected = core::selectAndAttachAgentTagRegistry(
			*building, buildingPath, registryPath);
		require(building->getAgentTagRegistryFilename() == "shared.tags.yaml",
			"Selection did not store only the registry basename");
		require(selected->getUuid() == registry->getUuid(),
			"Selection attached a registry with the wrong UUID");

		auto refused = std::make_shared<core::Building>("Refused selection", 4, 2);
		auto const refusedPath = project / "refused.yaml";
		refused->saveTo(refusedPath.string());
		auto wrongExtension = core::AgentTagRegistry::create();
		auto const wrongExtensionPath = project / "registry.yaml";
		wrongExtension->saveTo(wrongExtensionPath.string());
		bool extensionRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*refused, refusedPath, wrongExtensionPath);
		}
		catch (std::exception const& error)
		{
			extensionRefused = std::string(error.what()).find(".tags.yaml")
				!= std::string::npos;
		}
		require(extensionRefused && !refused->hasAgentTagRegistryReference()
			&& !refused->isModified(),
			"A registry with the wrong extension disturbed the Building");

		auto outside = core::AgentTagRegistry::create();
		auto const outsidePath = otherProject / "outside.tags.yaml";
		outside->saveTo(outsidePath.string());
		bool directoryRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*refused, refusedPath, outsidePath);
		}
		catch (std::exception const& error)
		{
			directoryRefused = std::string(error.what()).find("same directory")
				!= std::string::npos;
		}
		require(directoryRefused && !refused->hasAgentTagRegistryReference()
			&& !refused->isModified(),
			"A registry outside the Building directory disturbed the Building");
	}

	void canonicalFilesShareOneInstanceAndDirectoriesRemainDistinct()
	{
		TemporaryDirectory temporary;
		auto const firstDirectory = temporary.path / "first";
		auto const secondDirectory = temporary.path / "second";
		std::filesystem::create_directories(firstDirectory);
		std::filesystem::create_directories(secondDirectory);

		auto firstDiskRegistry = core::AgentTagRegistry::create();
		auto secondDiskRegistry = core::AgentTagRegistry::create();
		auto const firstRegistryPath = firstDirectory / "shared.tags.yaml";
		auto const secondRegistryPath = secondDirectory / "shared.tags.yaml";
		firstDiskRegistry->saveTo(firstRegistryPath.string());
		secondDiskRegistry->saveTo(secondRegistryPath.string());

		auto first = std::make_shared<core::Building>("First", 4, 2);
		auto second = std::make_shared<core::Building>("Second", 4, 2);
		auto third = std::make_shared<core::Building>("Third", 4, 2);
		auto const firstPath = firstDirectory / "first.yaml";
		auto const secondPath = firstDirectory / "second.yaml";
		auto const thirdPath = secondDirectory / "third.yaml";
		first->saveTo(firstPath.string());
		second->saveTo(secondPath.string());
		third->saveTo(thirdPath.string());

		auto sharedFirst = core::selectAndAttachAgentTagRegistry(
			*first, firstPath, firstRegistryPath);
		auto sharedSecond = core::selectAndAttachAgentTagRegistry(
			*second, secondPath, firstDirectory / "." / "shared.tags.yaml");
		auto distinct = core::selectAndAttachAgentTagRegistry(
			*third, thirdPath, secondRegistryPath);
		require(sharedFirst == sharedSecond,
			"One canonical registry file produced multiple loaded instances");
		require(sharedFirst != distinct,
			"Same-named registries in different directories shared an instance");

		first->saveTo(firstPath.string());
		second->saveTo(secondPath.string());
		first.reset();
		second.reset();
		sharedFirst.reset();
		sharedSecond.reset();

		auto reopenedFirst = core::loadBuildingDocument(firstPath);
		auto reopenedSecond = core::loadBuildingDocument(secondPath);
		require(reopenedFirst->getAgentTagRegistry()
			== reopenedSecond->getAgentTagRegistry(),
			"Two reopened Buildings did not share their canonical registry instance");
	}

	void duplicateUuidAndInvalidDocumentsAreTransactional()
	{
		TemporaryDirectory temporary;
		auto const firstDirectory = temporary.path / "first";
		auto const secondDirectory = temporary.path / "second";
		std::filesystem::create_directories(firstDirectory);
		std::filesystem::create_directories(secondDirectory);

		auto sourceRegistry = core::AgentTagRegistry::create();
		auto const sourcePath = firstDirectory / "registry.tags.yaml";
		auto const duplicatePath = secondDirectory / "registry.tags.yaml";
		sourceRegistry->saveTo(sourcePath.string());
		std::filesystem::copy_file(sourcePath, duplicatePath);

		auto first = std::make_shared<core::Building>("First", 4, 2);
		auto second = std::make_shared<core::Building>("Second", 4, 2);
		auto const firstPath = firstDirectory / "first.yaml";
		auto const secondPath = secondDirectory / "second.yaml";
		first->saveTo(firstPath.string());
		second->saveTo(secondPath.string());
		auto loadedFirst = core::selectAndAttachAgentTagRegistry(
			*first, firstPath, sourcePath);

		bool duplicateRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*second, secondPath, duplicatePath);
		}
		catch (std::exception const& error)
		{
			duplicateRefused = std::string(error.what()).find("already loaded")
				!= std::string::npos;
		}
		require(duplicateRefused && !second->hasAgentTagRegistryReference()
			&& !second->isModified()
			&& first->getAgentTagRegistry() == loadedFirst,
			"A duplicate registry UUID disturbed loaded document state");

		// A refusal must not cache the duplicate path. Replacing that path with a
		// genuinely independent registry makes it immediately selectable.
		auto independent = core::AgentTagRegistry::create();
		independent->saveTo(duplicatePath.string());
		auto loadedSecond = core::selectAndAttachAgentTagRegistry(
			*second, secondPath, duplicatePath);
		require(loadedSecond != loadedFirst,
			"A duplicate-UUID refusal left a stale registry loaded at its path");

		auto malformed = std::make_shared<core::Building>("Malformed", 4, 2);
		auto const malformedBuildingPath = firstDirectory / "malformed.yaml";
		auto const malformedRegistryPath = firstDirectory / "malformed.tags.yaml";
		malformed->saveTo(malformedBuildingPath.string());
		writeText(malformedRegistryPath, "agentTagRegistry: [not valid");
		bool malformedRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*malformed, malformedBuildingPath, malformedRegistryPath);
		}
		catch (std::exception const& error)
		{
			malformedRefused = std::string(error.what()).find("Could not load")
				!= std::string::npos;
		}
		require(malformedRefused && !malformed->hasAgentTagRegistryReference()
			&& !malformed->isModified(),
			"A malformed registry disturbed the Building");

		auto unsupportedRegistry = core::AgentTagRegistry::create();
		unsupportedRegistry->saveTo(malformedRegistryPath.string());
		auto unsupportedYaml = readText(malformedRegistryPath);
		auto const version = unsupportedYaml.find("version: 1");
		require(version != std::string::npos,
			"The registry fixture did not contain schema version 1");
		unsupportedYaml.replace(version, std::string("version: 1").size(), "version: 2");
		writeText(malformedRegistryPath, unsupportedYaml);
		bool unsupportedRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*malformed, malformedBuildingPath, malformedRegistryPath);
		}
		catch (std::exception const& error)
		{
			unsupportedRefused = std::string(error.what()).find("Unsupported")
				!= std::string::npos;
		}
		require(unsupportedRefused && !malformed->hasAgentTagRegistryReference()
			&& !malformed->isModified(),
			"An unsupported registry schema disturbed the Building");

		std::filesystem::remove(malformedRegistryPath);
		bool missingRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*malformed, malformedBuildingPath, malformedRegistryPath);
		}
		catch (std::exception const& error)
		{
			missingRefused = std::string(error.what()).find("missing")
				!= std::string::npos;
		}
		require(missingRefused && !malformed->hasAgentTagRegistryReference()
			&& !malformed->isModified(),
			"A missing registry disturbed the Building");
	}

	void refusedBuildingLoadKeepsCurrentStateAndUnloadsCandidateRegistry()
	{
		TemporaryDirectory temporary;
		auto const sourceDirectory = temporary.path / "source";
		auto const destinationDirectory = temporary.path / "destination";
		std::filesystem::create_directories(sourceDirectory);
		std::filesystem::create_directories(destinationDirectory);
		auto const buildingPath = sourceDirectory / "referencing.yaml";
		auto const registryPath = sourceDirectory / "referencing.tags.yaml";

		{
			auto persisted = std::make_shared<core::Building>("Persisted", 4, 2);
			persisted->saveTo(buildingPath.string());
			(void)core::createAndAttachAgentTagRegistry(*persisted, buildingPath);
			persisted->saveTo(buildingPath.string());
		}

		auto replacement = core::AgentTagRegistry::create();
		replacement->saveTo(registryPath.string());
		auto const replacementUuid = replacement->getUuid();
		auto const copiedReplacement = destinationDirectory / "replacement.tags.yaml";
		std::filesystem::copy_file(registryPath, copiedReplacement);
		replacement.reset();

		auto current = std::make_shared<core::Building>("Current", 7, 3);
		auto const* currentIdentity = current.get();
		bool mismatchRefused{ false };
		try
		{
			current = core::loadBuildingDocument(buildingPath);
		}
		catch (std::exception const& error)
		{
			mismatchRefused = std::string(error.what()).find("UUID mismatch")
				!= std::string::npos;
		}
		require(mismatchRefused && current.get() == currentIdentity
			&& current->getName() == "Current" && current->getCellsWide() == 7,
			"A refused Building load replaced or changed the current Building");

		// The replacement was parsed solely for the failed load above. It must no
		// longer count as loaded, so the same UUID at this independent path is valid.
		auto destination = std::make_shared<core::Building>("Destination", 4, 2);
		auto const destinationPath = destinationDirectory / "destination.yaml";
		destination->saveTo(destinationPath.string());
		auto selected = core::selectAndAttachAgentTagRegistry(
			*destination, destinationPath, copiedReplacement);
		require(selected->getUuid() == replacementUuid,
			"A registry loaded only for a failed Building load remained referenced");
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
	selectionEnforcesBasenameExtensionAndDirectory();
	canonicalFilesShareOneInstanceAndDirectoriesRemainDistinct();
	duplicateUuidAndInvalidDocumentsAreTransactional();
	refusedBuildingLoadKeepsCurrentStateAndUnloadsCandidateRegistry();
	failedAtomicCreationLeavesNoReferenceOrFile();
}
