// Safe Agent tag registry detach/switch workflow checks for #140.

#include "TagsPanel.h"
#include "DocumentEdit.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Building.h"
#include "core/YamlSerializer.h"

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
				/ ("promethium-fermide-registry-change-" + std::to_string(
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

	bool restoreBuildingSnapshot(std::shared_ptr<core::Building>& building,
		std::filesystem::path const& buildingPath, bool redo)
	{
		auto current = captureDocumentSnapshot(building);
		std::shared_ptr<core::Building> restored;
		auto restore = [&restored, &buildingPath](DocumentSnapshot const& target)
		{
			restored = std::make_shared<core::Building>("Loading", 1, 1);
			auto reader = core::YamlSerializer::fromString(target.yaml);
			reader->deserialize();
			core::SerializationWorkData workData;
			if (!restored->deserialize(*reader, workData)) return false;
			core::loadAndAttachAgentTagRegistry(*restored, buildingPath);
			return true;
		};
		auto const succeeded = redo
			? gBuildingDocumentHistory.redo(std::move(current), restore)
			: gBuildingDocumentHistory.undo(std::move(current), restore);
		if (succeeded) building = std::move(restored);
		return succeeded;
	}

	struct Fixture
	{
		TemporaryDirectory temporary;
		std::filesystem::path buildingPath{ temporary.path / "building.yaml" };
		std::filesystem::path sourcePath{ temporary.path / "source.tags.yaml" };
		std::filesystem::path replacementPath{ temporary.path / "replacement.tags.yaml" };
		std::shared_ptr<core::Building> building{
			std::make_shared<core::Building>("Registry change", 10, 3) };
		std::shared_ptr<core::AgentTagRegistry> sourceRegistry;
		core::AgentTagId sourceTag{};
		core::AgentId firstAgent{};
		core::AgentId secondAgent{};

		explicit Fixture(bool assignTags)
		{
			auto source = core::AgentTagRegistry::create();
			sourceTag = source->addAgentTag("source");
			std::string diagnostic;
			require(source->addAgentTagWalkSpeedModifier(sourceTag, &diagnostic), diagnostic);
			require(source->setAgentTagWalkSpeedModifier(
				sourceTag, { 0.9f, 0.9f }, &diagnostic), diagnostic);
			require(source->addAgentTagHeightModifier(sourceTag, &diagnostic), diagnostic);
			require(source->setAgentTagHeightModifier(
				sourceTag, { 0.8f, 0.8f }, &diagnostic), diagnostic);
			source->saveTo(sourcePath.string());

			auto replacement = core::AgentTagRegistry::create();
			(void)replacement->addAgentTag("replacement");
			replacement->saveTo(replacementPath.string());

			auto const corridor = building->addCorridor(0, 0, 8);
			building->finishBuild();
			firstAgent = building->createAgent("First", corridor, 0, 1.5f);
			secondAgent = building->createAgent("Second", corridor, 0, 3.5f);
			building->pauseSimulation();
			building->saveTo(buildingPath.string());
			sourceRegistry = core::selectAndAttachAgentTagRegistry(
				*building, buildingPath, sourcePath);
			if (assignTags)
			{
				require(building->assignAgentTag(firstAgent, sourceTag, &diagnostic), diagnostic);
				require(building->assignAgentTag(secondAgent, sourceTag, &diagnostic), diagnostic);
			}
		}
	};

	void unusedRegistryChangesAreDirectAndUndoable()
	{
		Fixture fixture(false);
		auto& building = fixture.building;
		auto const sourceUuid = fixture.sourceRegistry->getUuid();
		auto const sourceText = readText(fixture.sourcePath);
		auto const replacementText = readText(fixture.replacementPath);
		gBuildingDocumentHistory.clear();
		gBuildingDocumentHistory.markSaved();

		std::string diagnostic;
		require(commitAgentTagRegistrySwitch(building, fixture.buildingPath.string(),
			fixture.replacementPath.string(), diagnostic),
			"An unused registry did not switch directly: " + diagnostic);
		require(building->getAgentTagRegistryFilename() == "replacement.tags.yaml"
			&& building->getAgentTagRegistry()->getUuid() != sourceUuid
			&& gBuildingDocumentHistory.undoCount() == 1,
			"A direct registry switch changed the wrong reference or undo history");
		require(restoreBuildingSnapshot(building, fixture.buildingPath, false)
			&& building->getExpectedAgentTagRegistryUuid() == sourceUuid,
			"Undo did not restore the original unused registry reference");
		require(restoreBuildingSnapshot(building, fixture.buildingPath, true)
			&& building->getAgentTagRegistryFilename() == "replacement.tags.yaml",
			"Redo did not restore the direct registry switch");

		gBuildingDocumentHistory.clear();
		require(commitAgentTagRegistryDetach(building, diagnostic),
			"An unused registry did not detach directly: " + diagnostic);
		require(!building->hasAgentTagRegistryReference()
			&& gBuildingDocumentHistory.undoCount() == 1,
			"Direct detachment did not clear the reference in one undoable edit");
		require(std::filesystem::is_regular_file(fixture.sourcePath)
			&& std::filesystem::is_regular_file(fixture.replacementPath)
			&& readText(fixture.sourcePath) == sourceText
			&& readText(fixture.replacementPath) == replacementText,
			"Detaching deleted, renamed, or rewrote a registry file");
		require(restoreBuildingSnapshot(building, fixture.buildingPath, false)
			&& building->getAgentTagRegistryFilename() == "replacement.tags.yaml",
			"Undo did not reattach the directly detached registry");
	}

	void directSwitchWithAssignmentsIsRefusedTransactionally()
	{
		Fixture fixture(true);
		gBuildingDocumentHistory.clear();
		auto const before = serializeBuilding(*fixture.building);
		auto const source = fixture.building->getAgentTagRegistry();
		auto const modified = fixture.building->isModified();

		std::string diagnostic;
		require(!commitAgentTagRegistrySwitch(fixture.building,
			fixture.buildingPath.string(), fixture.replacementPath.string(), diagnostic)
			&& diagnostic.find("assignments exist") != std::string::npos,
			"A direct used-registry switch was not refused with a useful diagnostic");
		require(serializeBuilding(*fixture.building) == before
			&& fixture.building->getAgentTagRegistry() == source
			&& fixture.building->isModified() == modified
			&& !gBuildingDocumentHistory.canUndo(),
			"A refused direct registry switch changed state or created undo history");
	}

	void confirmedSwitchClearsEverythingAndCancellationDoesNothing()
	{
		Fixture fixture(true);
		gBuildingDocumentHistory.clear();
		auto const before = serializeBuilding(*fixture.building);
		auto const sourceUuid = fixture.sourceRegistry->getUuid();
		auto const sourceText = readText(fixture.sourcePath);
		auto const replacementText = readText(fixture.replacementPath);
		require(fixture.building->getAgentTagAssignmentCount() == 2
			&& fixture.building->getAgentTagAssignedAgentCount() == 2
			&& fixture.building->getAgentTagSampleCount() == 4,
			"The destructive-switch fixture did not contain every expected tag value");

		requestAgentTagRegistrySwitch(fixture.building,
			fixture.buildingPath.string(), fixture.replacementPath.string());
		std::string consequence;
		require(agentTagRegistryChangePending(&consequence)
			&& consequence.find("remove all 2 Agent tag assignments") != std::string::npos
			&& consequence.find("clear all 4 sampled Agent properties") != std::string::npos
			&& consequence.find("replacement.tags.yaml") != std::string::npos,
			"The destructive switch did not list its assignment, sample, and replacement consequences");
		cancelPendingAgentTagRegistryChange();
		require(!agentTagRegistryChangePending()
			&& serializeBuilding(*fixture.building) == before
			&& fixture.building->getExpectedAgentTagRegistryUuid() == sourceUuid
			&& !gBuildingDocumentHistory.canUndo(),
			"Cancelling a destructive registry switch changed state or history");

		// Confirming cannot make an invalid replacement less transactional: the
		// replacement is validated before any assignment or sample is cleared.
		auto const missingPath = fixture.temporary.path / "missing.tags.yaml";
		requestAgentTagRegistrySwitch(fixture.building,
			fixture.buildingPath.string(), missingPath.string());
		std::string diagnostic;
		require(!confirmPendingAgentTagRegistryChange(diagnostic)
			&& serializeBuilding(*fixture.building) == before
			&& fixture.building->getExpectedAgentTagRegistryUuid() == sourceUuid
			&& !gBuildingDocumentHistory.canUndo(),
			"A failed confirmed switch partially cleared tag state or created history");

		requestAgentTagRegistrySwitch(fixture.building,
			fixture.buildingPath.string(), fixture.replacementPath.string());
		require(confirmPendingAgentTagRegistryChange(diagnostic),
			"The confirmed destructive registry switch failed: " + diagnostic);
		require(fixture.building->getAgentTagRegistryFilename()
				== "replacement.tags.yaml"
			&& fixture.building->getExpectedAgentTagRegistryUuid() != sourceUuid
			&& fixture.building->getAgentTagAssignmentCount() == 0
			&& fixture.building->getAgentTagSampleCount() == 0
			&& fixture.building->getAgentTags(fixture.firstAgent).empty()
			&& fixture.building->getAgentTags(fixture.secondAgent).empty()
			&& !fixture.building->lookupAgent(fixture.firstAgent).entity
				->getWalkSpeedModifierSample()
			&& !fixture.building->lookupAgent(fixture.firstAgent).entity
				->getHeightModifierSample()
			&& !fixture.building->lookupAgent(fixture.secondAgent).entity
				->getWalkSpeedModifierSample()
			&& !fixture.building->lookupAgent(fixture.secondAgent).entity
				->getHeightModifierSample()
			&& gBuildingDocumentHistory.undoCount() == 1,
			"The confirmed switch did not atomically clear every assignment and sample");
		require(readText(fixture.sourcePath) == sourceText
			&& readText(fixture.replacementPath) == replacementText,
			"Switching rewrote one of the registry documents");

		require(restoreBuildingSnapshot(fixture.building, fixture.buildingPath, false)
			&& serializeBuilding(*fixture.building) == before,
			"Undo did not restore the original registry, assignments, and exact samples");

		gBuildingDocumentHistory.clear();
		requestAgentTagRegistryDetach(fixture.building);
		require(agentTagRegistryChangePending(&consequence)
			&& consequence.find("detach source.tags.yaml") != std::string::npos
			&& consequence.find("will not be deleted or renamed") != std::string::npos,
			"Destructive detachment did not state its file-safe consequence");
		require(confirmPendingAgentTagRegistryChange(diagnostic),
			"The confirmed destructive detachment failed: " + diagnostic);
		require(!fixture.building->hasAgentTagRegistryReference()
			&& fixture.building->getAgentTagAssignmentCount() == 0
			&& fixture.building->getAgentTagSampleCount() == 0
			&& std::filesystem::is_regular_file(fixture.sourcePath)
			&& readText(fixture.sourcePath) == sourceText,
			"Confirmed detachment retained tag state or changed the registry file");
	}
}

void runAgentTagRegistryChangeSmokeChecks()
{
	unusedRegistryChangesAreDirectAndUndoable();
	directSwitchWithAssignmentsIsRefusedTransactionally();
	confirmedSwitchClearsEverythingAndCancellationDoesNothing();
	cancelPendingAgentTagRegistryChange();
	gBuildingDocumentHistory.clear();
}
