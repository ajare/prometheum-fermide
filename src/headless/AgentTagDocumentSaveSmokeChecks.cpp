// Dependency-ordered Agent tag registry and Building saves, ticket #142.

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
	closePromptNamesOnlyTheDirtyDocumentKinds();
}
