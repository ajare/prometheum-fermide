// Dependency-ordered saves and independent cross-directory Save As copies,
// tickets #142 and #144.

#include "TagsPanel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Building.h"
#include "core/YamlSerializer.h"

void runAgentTagDocumentSaveSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path;

		TemporaryDirectory()
		{
			path = std::filesystem::temp_directory_path()
				/ ("promethium-fermide-document-save-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}

		~TemporaryDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	};

	std::string readFile(std::filesystem::path const& path)
	{
		std::ifstream input(path, std::ios::binary);
		return { std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>() };
	}

	std::string serializeRegistry(core::AgentTagRegistry const& registry)
	{
		auto serializer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		registry.serialize(*serializer, work);
		serializer->serialize();
		return serializer->getSerializedString();
	}

	std::string serializeBuilding(core::Building const& building)
	{
		auto serializer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		building.serialize(*serializer, work);
		serializer->serialize();
		return serializer->getSerializedString();
	}

	struct SavedFixture
	{
		TemporaryDirectory temporary;
		std::filesystem::path buildingPath;
		std::filesystem::path registryPath;
		std::shared_ptr<core::Building> building;
		std::shared_ptr<core::AgentTagRegistry> registry;
		DocumentHistory buildingHistory;
		core::AgentTagId tag{};

		explicit SavedFixture(std::string const& stem)
			: buildingPath(temporary.path / (stem + ".yaml")),
			registryPath(temporary.path / (stem + ".tags.yaml")),
			building(std::make_shared<core::Building>(stem, 8, 2))
		{
			auto const corridor = building->addCorridor(0, 0, 7);
			building->finishBuild();
			building->saveTo(buildingPath.string());
			registry = core::createAndAttachAgentTagRegistry(*building, buildingPath);
			building->pauseSimulation();

			std::string diagnostic;
			tag = commitAgentTagAdd(registry, "walkers", diagnostic);
			require(tag && commitAgentTagWalkSpeedModifierAdd(
				registry, tag, diagnostic), diagnostic);
			auto const agent = building->createAgent("Walker", corridor, 0, 2.0f);
			require(building->assignAgentTag(agent, tag, &diagnostic), diagnostic);
			require(saveBuildingDocument(target(), &diagnostic),
				"Could not establish a clean save fixture: " + diagnostic);
			require(!agentTagRegistryIsModified(registry)
				&& !building->isModified() && !buildingHistory.isModified(),
				"The initial registry and Building save did not clean independent markers");
		}

		~SavedFixture()
		{
			forgetAgentTagRegistryDocument(registry);
		}

		BuildingDocumentSaveTarget target()
		{
			return { building, buildingPath.string(), registryPath.string(),
				&buildingHistory };
		}

		void editModifier(float value)
		{
			std::string diagnostic;
			require(commitAgentTagWalkSpeedModifierEdit(
				registry, tag, { value, value }, diagnostic), diagnostic);
			buildingHistory.commit(buildingHistory.capture("Building before edit"));
			require(agentTagRegistryIsModified(registry)
				&& building->isModified() && buildingHistory.isModified(),
				"A modifier edit did not dirty registry and Building markers");
		}
	};

	void buildingSaveWritesRegistryFirstAndCleansIndependently()
	{
		SavedFixture fixture("single");
		fixture.editModifier(1.1f);
		std::string diagnostic;
		require(saveBuildingDocument(fixture.target(), &diagnostic), diagnostic);
		require(!agentTagRegistryIsModified(fixture.registry)
			&& !fixture.building->isModified()
			&& !fixture.buildingHistory.isModified(),
			"A successful dependency-ordered save did not clean each document marker");

		auto diskRegistry = core::AgentTagRegistry::loadFrom(
			fixture.registryPath.string());
		auto reopened = core::loadBuildingDocument(fixture.buildingPath);
		require(reopened->hasAttachedAgentTagRegistry()
			&& diskRegistry->getAgentTagWalkSpeedModifier(fixture.tag)->range
				== core::AgentModifierRange{ 1.1f, 1.1f }
			&& !reopened->isModified(),
			"The saved modifier definition and dependent samples did not round-trip cleanly");
	}

	void registryFailureBlocksBuildingAndPreservesDirtyState()
	{
		SavedFixture fixture("failure");
		auto const buildingOnDisk = readFile(fixture.buildingPath);
		auto const registryOnDisk = readFile(fixture.registryPath);
		fixture.editModifier(1.1f);

		core::YamlSerializer::setWriteFailureAfterBytesForTesting(1);
		std::string diagnostic;
		auto const saved = saveBuildingDocument(fixture.target(), &diagnostic);
		core::YamlSerializer::setWriteFailureAfterBytesForTesting(0);
		require(!saved && diagnostic.find("Agent tag registry") != std::string::npos,
			"An injected registry failure was not reported as a registry save failure");
		require(readFile(fixture.registryPath) == registryOnDisk
			&& readFile(fixture.buildingPath) == buildingOnDisk,
			"A registry failure changed the registry or allowed its Building onto disk");
		require(agentTagRegistryIsModified(fixture.registry)
			&& fixture.building->isModified()
			&& fixture.buildingHistory.isModified(),
			"A registry failure cleared a registry or Building dirty marker");
	}

	void saveAllCompletesRegistryPhaseBeforeAnyBuilding()
	{
		SavedFixture first("first");
		SavedFixture second("second");
		first.editModifier(1.1f);
		second.editModifier(1.2f);
		auto const firstBuildingOnDisk = readFile(first.buildingPath);
		auto const secondBuildingOnDisk = readFile(second.buildingPath);

		auto firstTarget = first.target();
		auto secondTarget = second.target();
		// A directory cannot be atomically replaced by the registry YAML file. It
		// gives the second registry a deterministic failure after the first one has
		// succeeded, proving that neither Building is written between those saves.
		auto const refusedDestination = second.temporary.path / "unwritable-registry";
		std::filesystem::create_directory(refusedDestination);
		secondTarget.registryFilepath = refusedDestination.string();
		std::string diagnostic;
		require(!saveAllDocuments({ firstTarget, secondTarget }, &diagnostic),
			"Save All unexpectedly accepted an unwritable registry destination");
		require(!agentTagRegistryIsModified(first.registry)
			&& agentTagRegistryIsModified(second.registry),
			"Successful and failed registry saves did not update their markers independently");
		require(first.building->isModified() && second.building->isModified()
			&& first.buildingHistory.isModified()
			&& second.buildingHistory.isModified(),
			"A failed registry phase cleared a dependent Building marker");
		require(readFile(first.buildingPath) == firstBuildingOnDisk
			&& readFile(second.buildingPath) == secondBuildingOnDisk,
			"Save All wrote a Building before every dirty registry had succeeded");
	}

	void sameDirectorySaveAsRetainsRegistryReference()
	{
		SavedFixture fixture("same-directory");
		auto const sourceRegistry = fixture.registry;
		auto const sourceUuid = sourceRegistry->getUuid();
		auto const sourceFilename = fixture.building->getAgentTagRegistryFilename();
		auto const destination = fixture.temporary.path / "same-directory-copy.yaml";

		std::string diagnostic;
		require(saveBuildingDocument({ fixture.building, destination.string(),
			fixture.registryPath.string(), &fixture.buildingHistory }, &diagnostic),
			"Same-directory Save As failed: " + diagnostic);
		require(fixture.building->getAgentTagRegistry() == sourceRegistry
			&& fixture.building->getExpectedAgentTagRegistryUuid() == sourceUuid
			&& fixture.building->getAgentTagRegistryFilename() == sourceFilename,
			"Same-directory Save As changed the existing registry reference");

		auto reopened = core::loadBuildingDocument(destination);
		require(reopened->getAgentTagRegistry() == sourceRegistry
			&& reopened->getExpectedAgentTagRegistryUuid() == sourceUuid,
			"A same-directory Save As did not reopen against the shared source registry");
	}

	void crossDirectorySaveAsCopiesEquivalentIndependentRegistry()
	{
		SavedFixture fixture("cross-directory");
		std::string diagnostic;
		auto const retired = commitAgentTagAdd(fixture.registry, "retired", diagnostic);
		require(static_cast<bool>(retired), diagnostic);
		require(commitAgentTagColourAdd(fixture.registry, retired, diagnostic), diagnostic);
		require(commitAgentTagDelete(fixture.registry, retired, diagnostic), diagnostic);
		require(commitAgentTagColourAdd(fixture.registry, fixture.tag, diagnostic), diagnostic);
		require(commitAgentTagColourEdit(fixture.registry, fixture.tag,
			{ 12, 34, 56 }, diagnostic), diagnostic);
		require(commitAgentTagWalkSpeedModifierEdit(fixture.registry, fixture.tag,
			{ 1.1f, 1.1f }, diagnostic), diagnostic);
		require(saveBuildingDocument(fixture.target(), &diagnostic), diagnostic);

		auto const sourceRegistry = fixture.registry;
		auto const sourceUuid = sourceRegistry->getUuid();
		auto const sourceRegistryYaml = serializeRegistry(*sourceRegistry);
		auto const sourceBuildingYaml = serializeBuilding(*fixture.building);
		auto copiedBuilding = core::loadBuildingDocument(fixture.buildingPath);
		copiedBuilding->pauseSimulation();
		DocumentHistory copiedHistory;
		copiedHistory.markSaved();

		auto const destinationDirectory = fixture.temporary.path / "copy";
		std::filesystem::create_directory(destinationDirectory);
		auto const destinationBuilding = destinationDirectory / "renamed.yaml";
		auto const destinationRegistry = destinationDirectory
			/ fixture.building->getAgentTagRegistryFilename();
		require(saveBuildingDocument({ copiedBuilding, destinationBuilding.string(),
			fixture.registryPath.string(), &copiedHistory }, &diagnostic),
			"Cross-directory Save As failed: " + diagnostic);

		auto const copiedRegistry = copiedBuilding->getAgentTagRegistry();
		require(copiedRegistry && copiedRegistry != sourceRegistry
			&& copiedRegistry->getUuid() != sourceUuid,
			"Cross-directory Save As did not attach an independent registry UUID");
		require(std::filesystem::is_regular_file(destinationRegistry)
			&& copiedBuilding->getAgentTagRegistryFilename()
				== fixture.building->getAgentTagRegistryFilename()
			&& copiedBuilding->getExpectedAgentTagRegistryUuid()
				== copiedRegistry->getUuid(),
			"The copied Building does not reference its adjacent registry copy");
		require(copiedRegistry->hasEquivalentDefinitions(*sourceRegistry)
			&& copiedRegistry->getAgentTagIds() == sourceRegistry->getAgentTagIds()
			&& copiedRegistry->getNextAgentTagId()
				== sourceRegistry->getNextAgentTagId()
			&& copiedRegistry->getNextPropertyRevision()
				== sourceRegistry->getNextPropertyRevision(),
			"The registry copy lost tag identities, definitions, revisions, or allocator state");
		require(copiedBuilding->getAgentTagAssignmentCount()
			== fixture.building->getAgentTagAssignmentCount(),
			"The copied Building lost Agent tag assignments");

		auto reopenedCopy = core::loadBuildingDocument(destinationBuilding);
		reopenedCopy->pauseSimulation();
		require(reopenedCopy->getAgentTagRegistry() == copiedRegistry
			&& reopenedCopy->getAgentTagAssignmentCount()
				== copiedBuilding->getAgentTagAssignmentCount(),
			"The copied Building and registry did not round-trip together");

		require(commitAgentTagRename(copiedRegistry, fixture.tag,
			"commuters", diagnostic), diagnostic);
		require(commitAgentTagWalkSpeedModifierEdit(copiedRegistry, fixture.tag,
			{ 1.2f, 1.2f }, diagnostic), diagnostic);
		require(sourceRegistry->getAgentTagName(fixture.tag) == "walkers"
			&& sourceRegistry->getAgentTagWalkSpeedModifier(fixture.tag)->range
				== core::AgentModifierRange{ 1.1f, 1.1f }
			&& serializeRegistry(*sourceRegistry) == sourceRegistryYaml
			&& serializeBuilding(*fixture.building) == sourceBuildingYaml,
			"Editing the copied registry affected the original registry or Building");
	}

	void registryCollisionLeavesSourceAndDestinationUnchanged()
	{
		SavedFixture fixture("collision");
		std::string diagnostic;
		require(commitAgentTagRename(fixture.registry, fixture.tag,
			"commuters", diagnostic), diagnostic);
		fixture.building->markModified();
		fixture.buildingHistory.commit(
			fixture.buildingHistory.capture("Building before collision Save As"));

		auto const sourceRegistryOnDisk = readFile(fixture.registryPath);
		auto const sourceBuildingOnDisk = readFile(fixture.buildingPath);
		auto const sourceRegistryInMemory = serializeRegistry(*fixture.registry);
		auto const sourceBuildingInMemory = serializeBuilding(*fixture.building);
		auto const sourceRegistry = fixture.registry;
		auto const sourceHistoryState = fixture.buildingHistory.currentStateId();

		auto const destinationDirectory = fixture.temporary.path / "occupied";
		std::filesystem::create_directory(destinationDirectory);
		auto const destinationBuilding = destinationDirectory / "copy.yaml";
		auto const destinationRegistry = destinationDirectory
			/ fixture.building->getAgentTagRegistryFilename();
		{
			std::ofstream registryOutput(destinationRegistry, std::ios::binary);
			registryOutput << "occupied registry";
			std::ofstream buildingOutput(destinationBuilding, std::ios::binary);
			buildingOutput << "occupied building";
		}
		auto const destinationRegistryBefore = readFile(destinationRegistry);
		auto const destinationBuildingBefore = readFile(destinationBuilding);

		require(!saveBuildingDocument({ fixture.building,
			destinationBuilding.string(), fixture.registryPath.string(),
			&fixture.buildingHistory }, &diagnostic),
			"Save As unexpectedly overwrote an existing destination registry");
		require(diagnostic.find("already exists") != std::string::npos,
			"A destination registry collision did not produce a useful diagnostic");
		require(readFile(destinationRegistry) == destinationRegistryBefore
			&& readFile(destinationBuilding) == destinationBuildingBefore,
			"A registry collision changed destination state");
		require(readFile(fixture.registryPath) == sourceRegistryOnDisk
			&& readFile(fixture.buildingPath) == sourceBuildingOnDisk
			&& serializeRegistry(*fixture.registry) == sourceRegistryInMemory
			&& serializeBuilding(*fixture.building) == sourceBuildingInMemory
			&& fixture.building->getAgentTagRegistry() == sourceRegistry
			&& fixture.buildingHistory.currentStateId() == sourceHistoryState
			&& agentTagRegistryIsModified(fixture.registry)
			&& fixture.buildingHistory.isModified(),
			"A registry collision changed source disk, document, reference, or dirty state");
	}

	void closePromptNamesOnlyTheDirtyDocumentKinds()
	{
		SavedFixture fixture("prompt");
		std::string diagnostic;
		require(commitAgentTagRename(fixture.registry, fixture.tag,
			"commuters", diagnostic), diagnostic);
		auto prompt = unsavedDocumentPromptText(fixture.target());
		require(prompt.find("Agent tag registry: prompt.tags.yaml")
				!= std::string::npos
			&& prompt.find("\n- Building:") == std::string::npos,
			"The close prompt did not identify a registry-only dirty state separately");

		fixture.building->markModified();
		fixture.buildingHistory.commit(
			fixture.buildingHistory.capture("Building before prompt edit"));
		prompt = unsavedDocumentPromptText(fixture.target());
		require(prompt.find("Building: prompt.yaml") != std::string::npos
			&& prompt.find("Agent tag registry: prompt.tags.yaml")
				!= std::string::npos,
			"The close/exit prompt did not name both independently dirty documents");
	}
}

void runAgentTagDocumentSaveSmokeChecks()
{
	buildingSaveWritesRegistryFirstAndCleansIndependently();
	registryFailureBlocksBuildingAndPreservesDirtyState();
	saveAllCompletesRegistryPhaseBeforeAnyBuilding();
	sameDirectorySaveAsRetainsRegistryReference();
	crossDirectorySaveAsCopiesEquivalentIndependentRegistry();
	registryCollisionLeavesSourceAndDestinationUnchanged();
	closePromptNamesOnlyTheDirtyDocumentKinds();
}
