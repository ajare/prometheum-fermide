// Inherited Agent Colour, ticket #133. Everything here is CPU-side: registry
// persistence, Building validation, Selection-panel text, and the real Agent
// renderer's ImDrawList output are exercised without a window or GPU.

#include "AgentTagAssignmentPanel.h"
#include "Render.h"
#include "TagsPanel.h"
#include "UISettings.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "imgui/imgui.h"

#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/Building.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

extern core::Agent* gSelectedAgent;
extern UISettings gUISettings;

void runAgentColourSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeRegistry(core::AgentTagRegistry const& registry)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		registry.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::string serializeBuilding(core::Building const& building)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		building.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::AgentTagRegistry> deserializeRegistry(std::string const& yaml)
	{
		auto registry = core::AgentTagRegistry::create();
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(registry->deserialize(*reader, work), "The Agent tag registry did not deserialize");
		return registry;
	}

	std::shared_ptr<core::Building> deserializeBuilding(std::string const& yaml)
	{
		auto building = std::make_shared<core::Building>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(building->deserialize(*reader, work), "The Building did not deserialize");
		return building;
	}

	void colourIsUniqueRevisionedAndPersisted()
	{
		// The editor boundary carries exactly three byte channels. Every possible
		// byte survives the float working representation used by ColorEdit3.
		for (int value = 0; value < 256; ++value)
		{
			auto const channel = static_cast<uint8_t>(value);
			float rgb[3];
			core::agentColourToFloats({ channel, 0, 0 }, rgb);
			require(core::agentColourFromFloats(rgb).r == channel,
				"An Agent Colour red channel did not survive its editor round-trip");
			core::agentColourToFloats({ 0, channel, 0 }, rgb);
			require(core::agentColourFromFloats(rgb).g == channel,
				"An Agent Colour green channel did not survive its editor round-trip");
			core::agentColourToFloats({ 0, 0, channel }, rgb);
			require(core::agentColourFromFloats(rgb).b == channel,
				"An Agent Colour blue channel did not survive its editor round-trip");
		}

		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("crew");
		registry->markUnmodified();
		std::string diagnostic;

		require(registry->addAgentTagColour(tag, &diagnostic),
			"Default Colour addition failed: " + diagnostic);
		auto const* added = registry->getAgentTagColour(tag);
		require(added && added->value == core::EditorDefaultAgentColour
			&& added->revision == 1 && registry->getNextPropertyRevision() == 2,
			"Colour did not start at RGB (179, 77, 77) with revision 1");
		auto const afterAdd = serializeRegistry(*registry);
		require(!registry->addAgentTagColour(tag, &diagnostic)
			&& diagnostic.find("already has Colour") != std::string::npos
			&& serializeRegistry(*registry) == afterAdd,
			"A tag accepted a second Colour or changed on refusal");

		registry->markUnmodified();
		require(!registry->setAgentTagColour(tag, core::EditorDefaultAgentColour, &diagnostic)
			&& registry->getNextPropertyRevision() == 2 && !registry->isModified(),
			"An unchanged Colour consumed a revision or dirtied the registry");
		require(registry->setAgentTagColour(tag, { 12, 34, 56 }, &diagnostic),
			"A real Colour edit failed: " + diagnostic);
		auto const* edited = registry->getAgentTagColour(tag);
		require(edited && edited->value == (core::AgentColour{ 12, 34, 56 })
			&& edited->revision == 2 && registry->getNextPropertyRevision() == 3,
			"A real Colour edit did not allocate exactly one new revision");

		auto reopened = deserializeRegistry(serializeRegistry(*registry));
		auto const* persisted = reopened->getAgentTagColour(tag);
		require(persisted && *persisted == *edited
			&& reopened->getNextPropertyRevision() == 3,
			"Colour value, revision, or allocator did not survive persistence");
		require(reopened->removeAgentTagColour(tag, &diagnostic)
			&& reopened->getNextPropertyRevision() == 3,
			"Removing Colour unexpectedly consumed a revision");
		require(reopened->addAgentTagColour(tag, &diagnostic)
			&& reopened->getAgentTagColour(tag)->revision == 3
			&& reopened->getNextPropertyRevision() == 4,
			"Removing and re-adding Colour reused an old revision");

		// The persisted closed property set rejects both unknown types and a
		// duplicate Colour, rather than silently choosing one.
		auto unknown = YAML::Load(serializeRegistry(*reopened));
		unknown["tags"][0]["properties"][0]["type"] = "future-property";
		bool unknownRefused{ false };
		try { (void)deserializeRegistry(YAML::Dump(unknown)); }
		catch (std::exception const& error)
		{
			unknownRefused = std::string(error.what()).find("Unsupported Agent property type")
				!= std::string::npos;
		}
		require(unknownRefused, "An unknown persisted Agent property type was accepted");

		auto document = YAML::Load(serializeRegistry(*reopened));
		YAML::Node duplicate(YAML::NodeType::Map);
		duplicate["type"] = "colour";
		duplicate["revision"] = 3;
		duplicate["r"] = 179;
		duplicate["g"] = 77;
		duplicate["b"] = 77;
		document["tags"][0]["properties"].push_back(duplicate);
		bool duplicateRefused{ false };
		try { (void)deserializeRegistry(YAML::Dump(document)); }
		catch (std::exception const& error)
		{
			duplicateRefused = std::string(error.what()).find("more than one Colour")
				!= std::string::npos;
		}
		require(duplicateRefused, "Serialized duplicate Colour properties were accepted");
	}

	struct ColourFixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry{ core::AgentTagRegistry::create() };
		std::shared_ptr<core::Building> building{
			std::make_shared<core::Building>("Colours", 8, 2) };
		core::AgentTagId red{ registry->addAgentTag("red") };
		core::AgentTagId blue{ registry->addAgentTag("blue") };
		core::AgentTagId plain{ registry->addAgentTag("plain") };
		core::AgentId coloured{};
		core::AgentId fallback{};

		ColourFixture()
		{
			std::string diagnostic;
			require(registry->addAgentTagColour(red, &diagnostic), diagnostic);
			require(registry->setAgentTagColour(red, { 12, 34, 56 }, &diagnostic), diagnostic);
			building->attachAgentTagRegistry("colours.tags.yaml", registry);
			auto const corridor = building->addCorridor(0, 0, 7);
			building->finishBuild();
			coloured = building->createAgent("Coloured", corridor, 0, 1.5f);
			fallback = building->createAgent("Fallback", corridor, 0, 3.5f);
			building->pauseSimulation();
			require(building->assignAgentTag(coloured, red, &diagnostic), diagnostic);
		}
	};

	void assignmentAndPropertyAdditionConflictsAreAtomic()
	{
		ColourFixture fixture;
		std::string diagnostic;
		require(fixture.registry->addAgentTagColour(fixture.blue, &diagnostic), diagnostic);
		auto const beforeAssignment = serializeBuilding(*fixture.building);
		require(!fixture.building->assignAgentTag(
			fixture.coloured, fixture.blue, &diagnostic)
			&& diagnostic.find("Colour") != std::string::npos
			&& diagnostic.find("#red") != std::string::npos
			&& diagnostic.find("#blue") != std::string::npos
			&& serializeBuilding(*fixture.building) == beforeAssignment,
			"A conflicting Colour assignment was not refused atomically with both sources");

		// Assigning property-free tags is still unrestricted. Adding Colour to
		// one afterwards must preflight every loaded Agent before consuming a
		// revision or touching the tag.
		require(fixture.building->assignAgentTag(
			fixture.coloured, fixture.plain, &diagnostic), diagnostic);
		forgetAgentTagRegistryDocument(fixture.registry);
		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		auto const revisionBefore = fixture.registry->getNextPropertyRevision();
		auto const registryBefore = serializeRegistry(*fixture.registry);
		require(!commitAgentTagColourAdd(fixture.registry, fixture.plain, diagnostic)
			&& diagnostic.find("Colour") != std::string::npos
			&& diagnostic.find("Coloured") != std::string::npos
			&& diagnostic.find("#red") != std::string::npos
			&& fixture.registry->getNextPropertyRevision() == revisionBefore
			&& serializeRegistry(*fixture.registry) == registryBefore
			&& history.undoCount() == 0,
			"A conflicting Colour addition mutated the registry, revision, or history");
		forgetAgentTagRegistryDocument(fixture.registry);
	}

	void closedBuildingConflictsAreRejectedWhenTheRegistryIsResolved()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const first = registry->addAgentTag("first");
		auto const second = registry->addAgentTag("second");
		std::string yaml;
		{
			auto building = std::make_shared<core::Building>("Closed", 6, 2);
			building->attachAgentTagRegistry("closed.tags.yaml", registry);
			auto const corridor = building->addCorridor(0, 0, 5);
			building->finishBuild();
			auto const agent = building->createAgent("Conflict", corridor);
			building->pauseSimulation();
			std::string diagnostic;
			require(building->assignAgentTag(agent, first, &diagnostic)
				&& building->assignAgentTag(agent, second, &diagnostic), diagnostic);
			yaml = serializeBuilding(*building);
		}

		std::string diagnostic;
		require(registry->addAgentTagColour(first, &diagnostic)
			&& registry->addAgentTagColour(second, &diagnostic), diagnostic);
		auto reopened = deserializeBuilding(yaml);
		bool refused{ false };
		try { reopened->resolveAgentTagRegistry(registry); }
		catch (std::exception const& error)
		{
			auto const text = std::string(error.what());
			refused = text.find("Colour") != std::string::npos
				&& text.find("#first") != std::string::npos
				&& text.find("#second") != std::string::npos;
		}
		require(refused && !reopened->hasAttachedAgentTagRegistry(),
			"A closed Building with duplicate inherited Colour sources was attached");
	}

	void editorCommitsRevisionedColourAndUndoRedoExactly()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("crew");
		forgetAgentTagRegistryDocument(registry);
		(void)agentTagRegistryDocumentHistory(registry);
		std::string diagnostic;
		require(commitAgentTagColourAdd(registry, tag, diagnostic), diagnostic);
		require(commitAgentTagColourEdit(registry, tag, { 4, 80, 160 }, diagnostic), diagnostic);
		auto const edited = *registry->getAgentTagColour(tag);
		auto const undoCount = agentTagRegistryDocumentHistory(registry).undoCount();
		require(!commitAgentTagColourEdit(registry, tag, edited.value, diagnostic)
			&& agentTagRegistryDocumentHistory(registry).undoCount() == undoCount
			&& registry->getNextPropertyRevision() == edited.revision + 1,
			"A no-op editor Colour submission created history or consumed a revision");
		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
		auto const* undone = registry->getAgentTagColour(tag);
		require(undone && undone->value == core::EditorDefaultAgentColour
			&& undone->revision == 1 && registry->getNextPropertyRevision() == 3,
			"Undo did not restore the exact initial Colour while retaining the revision high-water mark");
		require(restoreAgentTagRegistrySnapshot(registry, true, &diagnostic), diagnostic);
		require(*registry->getAgentTagColour(tag) == edited,
			"Redo did not restore the exact edited Colour and revision");
		forgetAgentTagRegistryDocument(registry);
	}

	void conflictingColourRedoIsRefusedAtomically()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const source = registry->addAgentTag("source");
		auto const target = registry->addAgentTag("target");
		std::string diagnostic;
		require(registry->addAgentTagColour(source, &diagnostic), diagnostic);
		forgetAgentTagRegistryDocument(registry);
		(void)agentTagRegistryDocumentHistory(registry);
		require(commitAgentTagColourAdd(registry, target, diagnostic), diagnostic);

		auto building = std::make_shared<core::Building>("Redo conflict", 6, 2);
		building->attachAgentTagRegistry("redo.tags.yaml", registry);
		auto const corridor = building->addCorridor(0, 0, 5);
		building->finishBuild();
		auto const agent = building->createAgent("Redo Agent", corridor);
		building->pauseSimulation();
		require(building->assignAgentTag(agent, target, &diagnostic), diagnostic);
		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic)
			&& !registry->getAgentTagColour(target),
			"Undo did not remove the newly added Colour: " + diagnostic);
		require(building->assignAgentTag(agent, source, &diagnostic), diagnostic);

		auto const registryBefore = serializeRegistry(*registry);
		auto const buildingBefore = serializeBuilding(*building);
		require(!restoreAgentTagRegistrySnapshot(registry, true, &diagnostic)
			&& diagnostic.find("Colour") != std::string::npos
			&& serializeRegistry(*registry) == registryBefore
			&& serializeBuilding(*building) == buildingBefore,
			"Redo introduced a conflicting Colour or partially changed loaded state");
		forgetAgentTagRegistryDocument(registry);
	}

	void captureClipboardText(void* userData, char const* text)
	{
		if (auto* writes = static_cast<std::vector<std::string>*>(userData))
			writes->emplace_back(text ? text : "");
	}

	char const* readClipboardText(void*) { return nullptr; }

	bool drawListContainsColour(ImDrawList const* drawList, ImU32 colour)
	{
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
			if (drawList->VtxBuffer[i].col == colour) return true;
		return false;
	}

	void effectiveInspectionAndRealRenderingUseInheritedFallbackAndGold()
	{
		ColourFixture fixture;
		auto const coloured = fixture.building->lookupAgent(fixture.coloured).entity;
		auto const fallback = fixture.building->lookupAgent(fixture.fallback).entity;
		auto const effective = coloured->getEffectiveColour();
		require(effective.value == (core::AgentColour{ 12, 34, 56 })
			&& effective.sourceTag == fixture.red,
			"The Agent did not expose its inherited Colour and source tag");
		require(fallback->getEffectiveColour().value == core::EditorDefaultAgentColour
			&& !fallback->getEffectiveColour().sourceTag,
			"An uncoloured Agent did not expose the editor fallback");

		require(agentRenderColour(*coloured, false) == ImU32(ImColor(12, 34, 56))
			&& agentRenderColour(*fallback, false)
				== ImU32(ImColor(179, 77, 77))
			&& agentRenderColour(*coloured, true)
				== ImU32(ImColor(251, 188, 4)),
			"Agent render-colour resolution does not preserve inherited, fallback, and gold");

		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(800.0f, 600.0f);
		io.Fonts->AddFontDefault();
		io.Fonts->Build();
		std::vector<std::string> clipboardWrites;
		io.SetClipboardTextFn = &captureClipboardText;
		io.GetClipboardTextFn = &readClipboardText;
		io.ClipboardUserData = &clipboardWrites;
		gUISettings.worldViewportHeight = 600.0f;

		ImGui::NewFrame();
		auto* drawList = ImGui::GetForegroundDrawList();
		gSelectedAgent = nullptr;
		renderAgent(coloured, drawList);
		renderAgent(fallback, drawList);
		gSelectedAgent = coloured;
		renderAgent(coloured, drawList);
		require(drawListContainsColour(drawList, ImU32(ImColor(12, 34, 56)))
			&& drawListContainsColour(drawList, ImU32(ImColor(179, 77, 77)))
			&& drawListContainsColour(drawList, ImU32(ImColor(251, 188, 4))),
			"The real Agent renderer did not emit inherited, fallback, and selected colours");
		gSelectedAgent = nullptr;
		ImGui::EndFrame();

		ImGui::NewFrame();
		ImGui::Begin("Selection");
		ImGui::LogToClipboard();
		renderAgentEffectiveProperties(fixture.building, fixture.coloured);
		renderAgentEffectiveProperties(fixture.building, fixture.fallback);
		ImGui::End();
		ImGui::Render();
		std::string visible;
		for (auto const& text : clipboardWrites) visible += text;
		require(visible.find("RGB (12, 34, 56) from #red") != std::string::npos
			&& visible.find("RGB (179, 77, 77) (editor default)") != std::string::npos,
			"The Selection panel omitted the effective Colour, source tag, or editor default");
		ImGui::DestroyContext();
	}
}

void runAgentColourSmokeChecks()
{
	colourIsUniqueRevisionedAndPersisted();
	assignmentAndPropertyAdditionConflictsAreAtomic();
	closedBuildingConflictsAreRejectedWhenTheRegistryIsResolved();
	editorCommitsRevisionedColourAndUndoRedoExactly();
	conflictingColourRedoIsRefusedAtomically();
	effectiveInspectionAndRealRenderingUseInheritedFallbackAndGold();
}
