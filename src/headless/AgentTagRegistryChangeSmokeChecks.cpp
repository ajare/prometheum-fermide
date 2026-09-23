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
#include "core/World.h"
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

	std::string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	bool restoreWorldSnapshot(std::shared_ptr<core::World>& world,
		std::filesystem::path const& worldPath, bool redo)
	{
		auto current = captureDocumentSnapshot(world);
		std::shared_ptr<core::World> restored;
		auto restore = [&restored, &worldPath](DocumentSnapshot const& target)
		{
			restored = std::make_shared<core::World>("Loading", 1, 1);
			auto reader = core::YamlSerializer::fromString(target.yaml);
			reader->deserialize();
			core::SerializationWorkData workData;
			if (!restored->deserialize(*reader, workData)) return false;
			core::loadAndAttachAgentTagRegistry(*restored, worldPath);
			return true;
		};
		auto const succeeded = redo
			? gWorldDocumentHistory.redo(std::move(current), restore)
			: gWorldDocumentHistory.undo(std::move(current), restore);
		if (succeeded) world = std::move(restored);
		return succeeded;
	}

	struct Fixture
	{
		TemporaryDirectory temporary;
		std::filesystem::path worldPath{ temporary.path / "world.world.yaml" };
		std::filesystem::path sourcePath{ temporary.path / "source.tags.yaml" };
		std::filesystem::path replacementPath{ temporary.path / "replacement.tags.yaml" };
		std::shared_ptr<core::World> world{
			std::make_shared<core::World>("Registry change", 10, 3) };
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

			auto const corridor = world->addCorridor(0, 0, 8);
			world->finishBuild();
			firstAgent = world->createAgent("First", corridor, 0, 1.5f);
			secondAgent = world->createAgent("Second", corridor, 0, 3.5f);
			world->pauseSimulation();
			world->saveTo(worldPath.string());
			sourceRegistry = core::selectAndAttachAgentTagRegistry(
				*world, worldPath, sourcePath);
			if (assignTags)
			{
				require(world->assignAgentTag(firstAgent, sourceTag, &diagnostic), diagnostic);
				require(world->assignAgentTag(secondAgent, sourceTag, &diagnostic), diagnostic);
			}
		}
	};

	void unusedRegistryChangesAreDirectAndUndoable()
	{
		Fixture fixture(false);
		auto& world = fixture.world;
		auto const sourceUuid = fixture.sourceRegistry->getUuid();
		auto const sourceText = readText(fixture.sourcePath);
		auto const replacementText = readText(fixture.replacementPath);
		gWorldDocumentHistory.clear();
		gWorldDocumentHistory.markSaved();

		std::string diagnostic;
		require(commitAgentTagRegistrySwitch(world, fixture.worldPath.string(),
			fixture.replacementPath.string(), diagnostic),
			"An unused registry did not switch directly: " + diagnostic);
		require(world->getAgentTagRegistryFilename() == "replacement.tags.yaml"
			&& world->getAgentTagRegistry()->getUuid() != sourceUuid
			&& gWorldDocumentHistory.undoCount() == 1,
			"A direct registry switch changed the wrong reference or undo history");
		require(restoreWorldSnapshot(world, fixture.worldPath, false)
			&& world->getExpectedAgentTagRegistryUuid() == sourceUuid,
			"Undo did not restore the original unused registry reference");
		require(restoreWorldSnapshot(world, fixture.worldPath, true)
			&& world->getAgentTagRegistryFilename() == "replacement.tags.yaml",
			"Redo did not restore the direct registry switch");

		gWorldDocumentHistory.clear();
		require(commitAgentTagRegistryDetach(world, diagnostic),
			"An unused registry did not detach directly: " + diagnostic);
		require(!world->hasAgentTagRegistryReference()
			&& gWorldDocumentHistory.undoCount() == 1,
			"Direct detachment did not clear the reference in one undoable edit");
		require(std::filesystem::is_regular_file(fixture.sourcePath)
			&& std::filesystem::is_regular_file(fixture.replacementPath)
			&& readText(fixture.sourcePath) == sourceText
			&& readText(fixture.replacementPath) == replacementText,
			"Detaching deleted, renamed, or rewrote a registry file");
		require(restoreWorldSnapshot(world, fixture.worldPath, false)
			&& world->getAgentTagRegistryFilename() == "replacement.tags.yaml",
			"Undo did not reattach the directly detached registry");
	}

	void directSwitchWithAssignmentsIsRefusedTransactionally()
	{
		Fixture fixture(true);
		gWorldDocumentHistory.clear();
		auto const before = serializeWorld(*fixture.world);
		auto const source = fixture.world->getAgentTagRegistry();
		auto const modified = fixture.world->isModified();

		std::string diagnostic;
		require(!commitAgentTagRegistrySwitch(fixture.world,
			fixture.worldPath.string(), fixture.replacementPath.string(), diagnostic)
			&& diagnostic.find("assignments exist") != std::string::npos,
			"A direct used-registry switch was not refused with a useful diagnostic");
		require(serializeWorld(*fixture.world) == before
			&& fixture.world->getAgentTagRegistry() == source
			&& fixture.world->isModified() == modified
			&& !gWorldDocumentHistory.canUndo(),
			"A refused direct registry switch changed state or created undo history");
	}

	void confirmedSwitchClearsEverythingAndCancellationDoesNothing()
	{
		Fixture fixture(true);
		gWorldDocumentHistory.clear();
		auto const before = serializeWorld(*fixture.world);
		auto const sourceUuid = fixture.sourceRegistry->getUuid();
		auto const sourceText = readText(fixture.sourcePath);
		auto const replacementText = readText(fixture.replacementPath);
		require(fixture.world->getAgentTagAssignmentCount() == 2
			&& fixture.world->getAgentTagAssignedAgentCount() == 2
			&& fixture.world->getAgentTagSampleCount() == 4,
			"The destructive-switch fixture did not contain every expected tag value");

		requestAgentTagRegistrySwitch(fixture.world,
			fixture.worldPath.string(), fixture.replacementPath.string());
		std::string consequence;
		require(agentTagRegistryChangePending(&consequence)
			&& consequence.find("remove all 2 Agent tag assignments") != std::string::npos
			&& consequence.find("clear all 4 sampled Agent properties") != std::string::npos
			&& consequence.find("replacement.tags.yaml") != std::string::npos,
			"The destructive switch did not list its assignment, sample, and replacement consequences");
		cancelPendingAgentTagRegistryChange();
		require(!agentTagRegistryChangePending()
			&& serializeWorld(*fixture.world) == before
			&& fixture.world->getExpectedAgentTagRegistryUuid() == sourceUuid
			&& !gWorldDocumentHistory.canUndo(),
			"Cancelling a destructive registry switch changed state or history");

		// Confirming cannot make an invalid replacement less transactional: the
		// replacement is validated before any assignment or sample is cleared.
		auto const missingPath = fixture.temporary.path / "missing.tags.yaml";
		requestAgentTagRegistrySwitch(fixture.world,
			fixture.worldPath.string(), missingPath.string());
		std::string diagnostic;
		require(!confirmPendingAgentTagRegistryChange(diagnostic)
			&& serializeWorld(*fixture.world) == before
			&& fixture.world->getExpectedAgentTagRegistryUuid() == sourceUuid
			&& !gWorldDocumentHistory.canUndo(),
			"A failed confirmed switch partially cleared tag state or created history");

		requestAgentTagRegistrySwitch(fixture.world,
			fixture.worldPath.string(), fixture.replacementPath.string());
		require(confirmPendingAgentTagRegistryChange(diagnostic),
			"The confirmed destructive registry switch failed: " + diagnostic);
		require(fixture.world->getAgentTagRegistryFilename()
				== "replacement.tags.yaml"
			&& fixture.world->getExpectedAgentTagRegistryUuid() != sourceUuid
			&& fixture.world->getAgentTagAssignmentCount() == 0
			&& fixture.world->getAgentTagSampleCount() == 0
			&& fixture.world->getAgentTags(fixture.firstAgent).empty()
			&& fixture.world->getAgentTags(fixture.secondAgent).empty()
			&& !fixture.world->lookupAgent(fixture.firstAgent).entity
				->getWalkSpeedModifierSample()
			&& !fixture.world->lookupAgent(fixture.firstAgent).entity
				->getHeightModifierSample()
			&& !fixture.world->lookupAgent(fixture.secondAgent).entity
				->getWalkSpeedModifierSample()
			&& !fixture.world->lookupAgent(fixture.secondAgent).entity
				->getHeightModifierSample()
			&& gWorldDocumentHistory.undoCount() == 1,
			"The confirmed switch did not atomically clear every assignment and sample");
		require(readText(fixture.sourcePath) == sourceText
			&& readText(fixture.replacementPath) == replacementText,
			"Switching rewrote one of the registry documents");

		require(restoreWorldSnapshot(fixture.world, fixture.worldPath, false)
			&& serializeWorld(*fixture.world) == before,
			"Undo did not restore the original registry, assignments, and exact samples");

		gWorldDocumentHistory.clear();
		requestAgentTagRegistryDetach(fixture.world);
		require(agentTagRegistryChangePending(&consequence)
			&& consequence.find("detach source.tags.yaml") != std::string::npos
			&& consequence.find("will not be deleted or renamed") != std::string::npos,
			"Destructive detachment did not state its file-safe consequence");
		require(confirmPendingAgentTagRegistryChange(diagnostic),
			"The confirmed destructive detachment failed: " + diagnostic);
		require(!fixture.world->hasAgentTagRegistryReference()
			&& fixture.world->getAgentTagAssignmentCount() == 0
			&& fixture.world->getAgentTagSampleCount() == 0
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
	gWorldDocumentHistory.clear();
}
