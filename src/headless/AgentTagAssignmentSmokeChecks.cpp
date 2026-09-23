// Property-free Agent tag assignments, ticket #131.

#include "AgentTagAssignmentPanel.h"
#include "AgentClipboard.h"
#include "DocumentEdit.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

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
				/ ("promethium-fermide-tag-assignments-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}
		~TemporaryDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	std::string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::World> deserializeWorld(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData workData;
		require(world->deserialize(*reader, workData), "The World did not deserialize");
		return world;
	}

	void writeText(std::filesystem::path const& path, std::string const& text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output) throw std::runtime_error("Could not write an Agent tag fixture");
	}

	struct Fixture
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<core::AgentTagRegistry> registry;
		core::AgentTagId crew;
		core::AgentTagId night;
		core::AgentId alice;
		uint32_t corridor;

		Fixture()
			: world(std::make_shared<core::World>("Tag assignments", 10, 3))
			, registry(core::AgentTagRegistry::create())
		{
			crew = registry->addAgentTag("crew");
			night = registry->addAgentTag("night-shift");
			world->attachAgentTagRegistry("shared.tags.yaml", registry);
			corridor = world->addCorridor(0, 0, 8);
			world->finishBuild();
			alice = world->createAgent("Alice", corridor, 0, 1.5f);
		}
	};

	void assignmentsAreUniquePausedOnlyAndTransactional()
	{
		Fixture fixture;
		std::string diagnostic;
		require(fixture.world->getAgentTags(fixture.alice).empty(),
			"A newly created Agent did not start untagged");

		require(!fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic)
			&& !diagnostic.empty(),
			"An Agent tag was assigned while the simulation was running");
		require(fixture.world->getAgentTags(fixture.alice).empty(),
			"A running-simulation refusal partially assigned a tag");

		fixture.world->pauseSimulation();
		require(fixture.world->assignAgentTag(fixture.alice, fixture.night, &diagnostic)
			&& fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic),
			"Two distinct property-free Agent tags could not be assigned");
		auto const expected = std::set<core::AgentTagId>{ fixture.crew, fixture.night };
		require(fixture.world->getAgentTags(fixture.alice) == expected,
			"Agent tag assignments are not exposed as one stable set");

		auto const beforeRefusals = serializeWorld(*fixture.world);
		require(!fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic),
			"A duplicate Agent tag assignment was accepted");
		require(!fixture.world->assignAgentTag(core::AgentId{ 9999 }, fixture.crew, &diagnostic),
			"An unknown Agent received a tag");
		require(!fixture.world->assignAgentTag(fixture.alice, core::AgentTagId{ 9999 }, &diagnostic),
			"An unknown Agent tag was assigned");
		require(!fixture.world->removeAgentTag(fixture.alice, core::AgentTagId{ 9999 }, &diagnostic),
			"An unknown Agent tag was removed");
		require(serializeWorld(*fixture.world) == beforeRefusals,
			"A refused assignment operation partially mutated the World");

		require(fixture.world->removeAgentTag(fixture.alice, fixture.crew, &diagnostic)
			&& !fixture.world->lookupAgent(fixture.alice).entity->hasAgentTag(fixture.crew)
			&& fixture.world->lookupAgent(fixture.alice).entity->hasAgentTag(fixture.night),
			"An assigned tag was not independently removable");

		auto noRegistry = std::make_shared<core::World>("No registry", 6, 2);
		auto const corridor = noRegistry->addCorridor(0, 0, 4);
		noRegistry->finishBuild();
		auto const agent = noRegistry->createAgent("No tags", corridor);
		noRegistry->pauseSimulation();
		require(!noRegistry->assignAgentTag(agent, fixture.crew, &diagnostic)
			&& diagnostic.find("registry") != std::string::npos
			&& noRegistry->getAgentTags(agent).empty(),
			"Assignment without a registry was not refused atomically");
	}

	void newAndPalettePlacedAgentsRemainUntagged()
	{
		Fixture fixture;
		fixture.world->pauseSimulation();
		std::string diagnostic;
		require(fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic),
			"The fixture Agent could not be tagged");

		auto const normal = fixture.world->createAgent("Bob", fixture.corridor, 0, 2.5f);
		require(fixture.world->getAgentTags(normal).empty(),
			"Normal Agent creation copied an existing Agent's tags");

		gWorldDocumentHistory.clear();
		core::AgentId placed{};
		auto const sector = fixture.world->getSector(fixture.corridor);
		require(commitAgentPlacement(fixture.world,
			AgentClipboardPayload{ "Palette Agent", 0, true, std::nullopt },
			sector, 0, 3.5f, placed, diagnostic),
			"The palette-equivalent Agent placement failed: " + diagnostic);
		require(placed && fixture.world->getAgentTags(placed).empty(),
			"A palette-placed Agent did not start untagged");
	}

	void assignmentsSerializeInNumericOrderAndRejectMalformedInput()
	{
		Fixture fixture;
		fixture.world->pauseSimulation();
		std::string diagnostic;
		// Deliberately assign in descending ID order.
		require(fixture.world->assignAgentTag(fixture.alice, fixture.night, &diagnostic)
			&& fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic),
			"The serialization fixture could not assign its tags");

		auto const yaml = serializeWorld(*fixture.world);
		auto document = YAML::Load(yaml);
		auto tags = document["agents"][0]["agent"]["tags"];
		require(tags && tags.IsSequence() && tags.size() == 2
			&& tags[0].as<uint64_t>() == fixture.crew.value
			&& tags[1].as<uint64_t>() == fixture.night.value,
			"Agent tag IDs did not serialize in stable numeric order:\n" + yaml);

		document["agents"][0]["agent"]["tags"].push_back(fixture.crew.value);
		bool duplicateRefused{ false };
		try { (void)deserializeWorld(YAML::Dump(document)); }
		catch (std::exception const& error)
		{
			duplicateRefused = std::string(error.what()).find("unique") != std::string::npos;
		}
		require(duplicateRefused, "Duplicate Agent tag IDs in input were not rejected");

		auto withoutRegistry = YAML::Load(yaml);
		withoutRegistry.remove("agentTagRegistry");
		bool absentRegistryRefused{ false };
		try { (void)deserializeWorld(YAML::Dump(withoutRegistry)); }
		catch (std::exception const& error)
		{
			absentRegistryRefused = std::string(error.what()).find("no Agent tag registry")
				!= std::string::npos;
		}
		require(absentRegistryRefused,
			"Serialized assignments without a registry reference were accepted");
	}

	void saveReopenAndUnknownTagValidationUseStableIds()
	{
		TemporaryDirectory temporary;
		Fixture fixture;
		fixture.world->pauseSimulation();
		std::string diagnostic;
		require(fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic)
			&& fixture.world->assignAgentTag(fixture.alice, fixture.night, &diagnostic),
			"The reopen fixture could not assign its tags");

		auto const registryPath = temporary.path / "shared.tags.yaml";
		auto const worldPath = temporary.path / "world.world.yaml";
		fixture.registry->saveTo(registryPath.string());
		fixture.world->saveTo(worldPath.string());

		auto reopened = core::loadWorldDocument(worldPath);
		auto const reopenedAgent = reopened->lookupAgent(fixture.alice);
		require(reopenedAgent
			&& reopenedAgent.entity->getAgentTagIds()
				== std::set<core::AgentTagId>{ fixture.crew, fixture.night },
			"Agent tag stable IDs did not survive save and reopen");

		auto malformed = YAML::Load(serializeWorld(*fixture.world));
		malformed["agents"][0]["agent"]["tags"][0] = 9999;
		auto const malformedPath = temporary.path / "malformed.world.yaml";
		writeText(malformedPath, YAML::Dump(malformed));
		// The registry basename in the fixture is shared.tags.yaml, so this file
		// resolves the same adjacent registry before validating assignments.
		bool unknownRefused{ false };
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const& error)
		{
			unknownRefused = std::string(error.what()).find("does not define")
				!= std::string::npos;
		}
		require(unknownRefused, "A serialized assignment to an unknown tag was accepted");
	}

	void editorCommitsOneWorldUndoEntryPerAcceptedEdit()
	{
		Fixture fixture;
		fixture.world->pauseSimulation();
		gWorldDocumentHistory.clear();
		std::string diagnostic;

		require(commitAgentTagAssignment(fixture.world, fixture.alice,
			fixture.crew, true, diagnostic), "The editor seam refused the first assignment");
		require(commitAgentTagAssignment(fixture.world, fixture.alice,
			fixture.night, true, diagnostic), "The editor seam refused the second assignment");
		require(gWorldDocumentHistory.undoCount() == 2,
			"Two accepted assignment edits did not commit two World undo entries");
		require(!commitAgentTagAssignment(fixture.world, fixture.alice,
			fixture.crew, true, diagnostic)
			&& gWorldDocumentHistory.undoCount() == 2,
			"A duplicate assignment committed a World undo entry");

		auto current = captureDocumentSnapshot(fixture.world);
		std::shared_ptr<core::World> restored;
		auto restore = [&](DocumentSnapshot const& target)
		{
			restored = deserializeWorld(target.yaml);
			restored->resolveAgentTagRegistry(fixture.registry);
			return true;
		};
		require(gWorldDocumentHistory.undo(std::move(current), restore),
			"Undo refused the accepted Agent tag assignment");
		fixture.world = restored;
		require(fixture.world->getAgentTags(fixture.alice)
			== std::set<core::AgentTagId>{ fixture.crew },
			"Undo did not remove exactly the last assigned tag");

		current = captureDocumentSnapshot(fixture.world);
		require(gWorldDocumentHistory.redo(std::move(current), restore),
			"Redo refused the Agent tag assignment");
		fixture.world = restored;
		require(fixture.world->getAgentTags(fixture.alice)
			== std::set<core::AgentTagId>{ fixture.crew, fixture.night },
			"Redo did not restore the two-tag assignment set");

		fixture.world->pauseSimulation();
		auto const entriesBeforeRemoval = gWorldDocumentHistory.undoCount();
		require(commitAgentTagAssignment(fixture.world, fixture.alice,
			fixture.crew, false, diagnostic)
			&& gWorldDocumentHistory.undoCount() == entriesBeforeRemoval + 1
			&& fixture.world->getAgentTags(fixture.alice)
				== std::set<core::AgentTagId>{ fixture.night },
			"Removing an assigned tag did not commit exactly one World undo entry");
	}

	void captureClipboardText(void* userData, char const* text)
	{
		if (auto* writes = static_cast<std::vector<std::string>*>(userData))
			writes->emplace_back(text ? text : "");
	}

	char const* readCapturedClipboardText(void*) { return nullptr; }

	void selectionPanelRendersAssignedChipsWithoutLeakingDisabledState()
	{
		Fixture fixture;
		fixture.world->pauseSimulation();
		std::string diagnostic;
		require(fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic),
			"The checklist fixture could not assign its removable tag");

		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(800.0f, 600.0f);
		io.Fonts->AddFontDefault();
		io.Fonts->Build();
		std::vector<std::string> clipboardWrites;
		io.SetClipboardTextFn = &captureClipboardText;
		io.GetClipboardTextFn = &readCapturedClipboardText;
		io.ClipboardUserData = &clipboardWrites;

		for (bool paused : { true, false })
		{
			if (paused) fixture.world->pauseSimulation();
			else require(fixture.world->resumeSimulation(),
				"The checklist fixture could not resume simulation");

			clipboardWrites.clear();
			ImGui::NewFrame();
			ImGui::Begin("Selection");
			ImGui::LogToClipboard();
			auto const disabledDepth = GImGui->DisabledStackSize;
			renderAgentTagAssignmentChecklist(fixture.world, fixture.alice);
			require(GImGui->DisabledStackSize == disabledDepth,
				"The Agent tag checklist leaked a disabled scope");
			ImGui::End();
			ImGui::Render();

			std::string visible;
			for (auto const& text : clipboardWrites) visible += text;
			require(visible.find("Agent tags") != std::string::npos
				&& visible.find("#crew") != std::string::npos
				&& visible.find("Add tag...") != std::string::npos,
				"The Selection panel did not present the assigned tag chip and add-tag combo");
			require(visible.find("#night-shift") == std::string::npos,
				"The Selection panel listed an unassigned tag outside the add-tag combo");
			require(fixture.world->getAgentTags(fixture.alice)
				== std::set<core::AgentTagId>{ fixture.crew },
				"Merely rendering the tag chips changed its assigned tag");
		}
		ImGui::DestroyContext();
	}
}

void runAgentTagAssignmentSmokeChecks()
{
	assignmentsAreUniquePausedOnlyAndTransactional();
	newAndPalettePlacedAgentsRemainUntagged();
	assignmentsSerializeInNumericOrderAndRejectMalformedInput();
	saveReopenAndUnknownTagValidationUseStableIds();
	editorCommitsOneWorldUndoEntryPerAcceptedEdit();
	selectionPanelRendersAssignedChipsWithoutLeakingDisabledState();
	gWorldDocumentHistory.clear();
}
