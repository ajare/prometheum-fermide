// Agent groups through copy, cut and paste, for ticket #113.
//
// The hazard this file circles is an ID crossing a document boundary. An
// AgentGroupId is a receipt for one Building's registry; handed to another
// Building it names nothing, or worse, something else. So the clipboard
// carries the Agent group by name (ADR 0006), and the destination resolves
// that name when the paste lands: reuse the group it already defines, or
// make one.
//
// The other hazard is a placement that never lands. A pasted Agent falls from
// the cursor and may be cancelled on the way down, and a paste whose group
// name turns out to be unusable should fail while the cursor is still where
// the user put it. Neither may leave an Agent, a group, or an undo entry
// behind.
//
// What gets pinned down:
//
//   a copied Agent's payload carries its Agent group by name and its
//   Building-local AgentGroupId nowhere
//   an ungrouped Agent's payload says nothing about a group at all, for a
//   copy and for a cut alike
//   a payload written before grouping existed - no `group` key - still
//   reads, and pastes an ungrouped Agent
//   a payload whose `group` is not a name is refused with a diagnostic,
//   not read as some other value
//   a paste reuses the destination's own group when it already defines
//   that exact name, and matches case-sensitively
//   a paste creates a missing group and assigns the Agent to it as one
//   document edit
//   arming a deferred placement writes nothing, and cancelling it leaves
//   nothing to land
//   a placement that fails creates neither Agent nor group and commits no
//   undo entry
//   an unusable group name is refused whole: not truncated to fit, not
//   quietly dropped, not half created
//   undo takes the pasted Agent and the group the paste made together, and
//   redo brings both back with the assignment
//   a cut takes the Agent and leaves its source Agent group defined, with
//   the other members still assigned

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "core/Agent.h"
#include "core/AgentGroup.h"
#include "core/Building.h"
#include "core/EntityId.h"
#include "core/Sector.h"
#include "core/SerializationWorkData.h"
#include "core/Simulation.h"
#include "core/YamlSerializer.h"

#include "AgentClipboard.h"
#include "DocumentEdit.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// The shared Building history outlives each check, so every check starts
	// from a clean instance rather than whatever the check before it left.
	void resetUndoHistory()
	{
		gBuildingDocumentHistory.clear();
	}

	// The editor's own undo and redo, the same shape as UI.cpp's
	// restoreDocumentSnapshot(): the live state crosses to the other stack
	// and the newest snapshot on the source stack becomes the live Building.
	void restoreDocument(std::shared_ptr<core::Building>& building, bool redo)
	{
		auto const current = captureDocumentSnapshot(building);
		require(current.has_value(), "The live document could not be captured");

		std::shared_ptr<core::Building> loaded;
		auto restore = [&loaded](DocumentSnapshot const& target)
		{
			loaded = std::make_shared<core::Building>("Restored Building", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(target.yaml);
			reader->deserialize();
			return loaded->deserialize(*reader, workData);
		};
		auto const restored = redo
			? gBuildingDocumentHistory.redo(current, restore)
			: gBuildingDocumentHistory.undo(current, restore);
		require(restored, redo ? "There is no redo entry to restore"
			: "There is no undo entry to restore");
		building = std::move(loaded);
	}

	// ---------------------------------------------------------------- world

	// Somewhere a paste may land and somewhere it must not: a Corridor and a
	// Room on the front Layer, and a Background behind them that owns no
	// walkable floor and refuses an Agent outright.
	struct PasteWorld
	{
		std::shared_ptr<core::Building> building;
		uint32_t corridor{ 0 };
		uint32_t room{ 0 };
		uint32_t background{ 0 };
	};

	PasteWorld buildPasteWorld(std::string const& name)
	{
		PasteWorld world;
		world.building = std::make_shared<core::Building>(name, 12, 3);
		world.corridor = world.building->addCorridor(0, 0, 12);
		world.room = world.building->addRoom("Workshop", 0, 2, 0, 6, 1);
		world.background = world.building->addBackground(1, 0, 0, 4, 1);
		world.building->finishBuild();
		return world;
	}

	std::size_t agentCount(core::Building const& building)
	{
		return building.getSimulationSnapshot().agents.size();
	}

	int countGroupsNamed(core::Building const& building, std::string const& name)
	{
		int count{ 0 };
		for (auto const id : building.getAgentGroupIds())
		{
			auto const group = building.lookupAgentGroup(id);
			if (group && group.entity->getName() == name) ++count;
		}
		return count;
	}

	core::AgentGroupId findGroupNamed(core::Building const& building,
		std::string const& name)
	{
		for (auto const id : building.getAgentGroupIds())
		{
			auto const group = building.lookupAgentGroup(id);
			if (group && group.entity->getName() == name) return id;
		}
		return {};
	}

	core::AgentId createAgent(core::Building& building, std::string const& name,
		uint32_t sector)
	{
		return building.createAgent(name, sector, 0, 2.0f);
	}

	void assign(core::Building& building, core::AgentId agent, core::AgentGroupId group)
	{
		std::string diagnostic;
		require(building.setAgentGroup(agent, group, &diagnostic),
			("Assigning an Agent for a clipboard check failed: " + diagnostic).c_str());
	}

	// ---------------------------------------------------------- clipboard

	// The clipboard text a copy of `agent` produces, written by the same
	// payload builder and text writer the editor's copy keystroke calls.
	std::string copyText(core::Building const& source, core::AgentId agent,
		std::string const& copyName, bool cut = false)
	{
		return makeAgentClipboardText(makeAgentClipboardPayload(source, agent, copyName), cut);
	}

	// The envelope read UI.cpp's parseClipboard performs, so a check can hand
	// the shared Agent reader a payload the same way the editor does - and so
	// a hand-written legacy payload is judged by the same envelope rules.
	struct ReadClipboard
	{
		AgentClipboardPayload payload;
		bool cut{ false };
	};

	ReadClipboard readClipboard(std::string const& text)
	{
		ReadClipboard read;
		auto const document = YAML::Load(text);
		auto const root = document["prometheumClipboard"];
		require(root && root.IsMap(),
			"The clipboard text carries no clipboard envelope");
		require(root["version"].as<std::uint32_t>() == 1,
			"The clipboard payload was written under an unsupported version");
		auto const operation = root["operation"].as<std::string>();
		require(operation == "copy" || operation == "cut",
			"The clipboard payload names an unsupported operation");
		require(root["type"].as<std::string>() == "Agent",
			"The clipboard payload is not an Agent");
		read.cut = operation == "cut";

		std::string diagnostic;
		require(readAgentClipboardObject(root["object"], read.payload, diagnostic),
			"Reading the Agent clipboard payload failed: " + diagnostic);
		return read;
	}

	// A payload written by hand, the way a payload from an older build
	// arrives: the envelope is there, and whatever the object map happened to
	// hold at the time.
	std::string legacyClipboardText(std::string const& objectLines)
	{
		return "prometheumClipboard:\n"
			"  version: 1\n"
			"  operation: copy\n"
			"  type: Agent\n"
			"  object:\n"
			+ objectLines;
	}

	// The keys the clipboard `object` map carries, sorted, so a check can
	// name exactly what a payload is allowed to hold.
	std::vector<std::string> clipboardObjectKeys(std::string const& text)
	{
		std::vector<std::string> keys;
		for (auto const& entry : YAML::Load(text)["prometheumClipboard"]["object"])
			keys.push_back(entry.first.as<std::string>());
		std::sort(keys.begin(), keys.end());
		return keys;
	}

	// ... and the values, as text, so a check can say that none of them is
	// some Building-local number.
	std::vector<std::string> clipboardObjectValues(std::string const& text)
	{
		std::vector<std::string> values;
		for (auto const& entry : YAML::Load(text)["prometheumClipboard"]["object"])
			values.push_back(entry.second.as<std::string>());
		return values;
	}

	// Land a payload at a target through the shared placement the editor's
	// pegman landing calls.
	core::AgentId place(std::shared_ptr<core::Building> const& building,
		AgentClipboardPayload const& payload, std::shared_ptr<const core::Sector> sector,
		uint32_t deckOffset = 0, float localX = 2.0f)
	{
		core::AgentId placed{};
		std::string diagnostic;
		require(commitAgentPlacement(building, payload, sector, deckOffset, localX,
			placed, diagnostic), "Placing the pasted Agent failed: " + diagnostic);
		require(!!placed, "The placement reported success without naming an Agent");
		return placed;
	}

	// ---------------------------------------------------------------- checks

	// The rule the whole ticket turns on: the name travels, the ID stays
	// home.
	void aCopiedAgentCarriesItsAgentGroupByNameAndNoLocalId()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Copy source");
		auto const crew = world.building->addAgentGroup("Crew");
		auto const alice = createAgent(*world.building, "Alice", world.room);
		assign(*world.building, alice, crew);

		auto const text = copyText(*world.building, alice, "Alice copy");
		auto const read = readClipboard(text);

		require(read.payload.name == "Alice copy",
			"The copied payload lost the copy's name");
		require(read.payload.group.has_value() && *read.payload.group == "Crew",
			"The copied payload did not carry the Agent group name");
		require(text.find("group: Crew") != std::string::npos,
			"The clipboard text does not name the Agent group: " + text);

		// No ID of any kind crosses. The clipboard object map holds a name,
		// flags and the Agent group name - nothing else - and none of those
		// values is the group's Building-local AgentGroupId, which would be
		// meaningless to whatever pastes this.
		auto const keys = clipboardObjectKeys(text);
		require(keys == std::vector<std::string>{ "flags", "group", "name" },
			"The clipboard payload carries more than a name, flags and an Agent"
			" group name: " + text);
		auto const values = clipboardObjectValues(text);
		require(std::find(values.begin(), values.end(), std::to_string(crew.value))
			== values.end(),
			"The clipboard payload carries the Agent group's local ID: " + text);
	}

	// An ungrouped Agent's payload is silent about classification rather
	// than saying "the empty one", which is what keeps it identical to a
	// payload written before grouping existed.
	void aCopiedUngroupedAgentCarriesNoAgentGroup()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Ungrouped source");
		auto const freelance = createAgent(*world.building, "Freelance", world.corridor);

		for (bool const cut : { false, true })
		{
			auto const text = copyText(*world.building, freelance, "Freelance copy", cut);
			require(text.find("group") == std::string::npos,
				"An ungrouped Agent's clipboard payload named a group anyway: " + text);

			auto const read = readClipboard(text);
			require(!read.payload.group.has_value(),
				"An ungrouped Agent's payload read back with a group");
			require(read.cut == cut,
				"The clipboard operation did not survive the round trip");

			core::AgentId placed{};
			std::string diagnostic;
			PendingAgentPlacement pending;
			require(armAgentPlacement(pending, *world.building, read.payload,
				world.building->getSector(world.corridor), 0, 3.0f, diagnostic),
				"Arming an ungrouped paste failed: " + diagnostic);
			require(commitPendingAgentPlacement(pending, world.building, placed, diagnostic),
				"Pasting an ungrouped Agent failed: " + diagnostic);
			require(!world.building->getAgentGroup(placed),
				"Pasting an ungrouped Agent gave it an Agent group");
			require(world.building->getAgentGroupCount() == 0,
				"Pasting an ungrouped Agent created an Agent group");
		}
	}

	// A payload from a build that never heard of Agent groups: no `group`
	// key, and nothing invented to fill the gap.
	void aLegacyPayloadWithoutAnAgentGroupStillPastes()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Legacy target");
		auto const before = agentCount(*world.building);

		auto const read = readClipboard(legacyClipboardText(
			"    name: Legacy hand\n"
			"    flags: 0\n"));
		require(!read.payload.group.has_value(),
			"A legacy payload without a group read back with one");
		require(read.payload.name == "Legacy hand",
			"A legacy payload lost its name");

		auto const placed = place(world.building, read.payload,
			world.building->getSector(world.corridor), 0, 4.0f);
		require(agentCount(*world.building) == before + 1,
			"The legacy paste did not add exactly one Agent");
		require(!world.building->getAgentGroup(placed),
			"A legacy paste gave the Agent an Agent group it never named");
		require(world.building->getAgentGroupCount() == 0,
			"A legacy paste created an Agent group out of an absent key");
	}

	// A `group` that is not a name is refused outright. Reading `group: 42`
	// as the name "42" would let a Building-local ID back in through the
	// side door, and reading a bad value as "no group" would drop the
	// classification without telling anyone.
	void aClipboardAgentGroupThatIsNotANameIsRefused()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Bad group target");

		std::vector<std::string> const bad = {
			legacyClipboardText("    name: Rescuer\n    flags: 0\n    group:\n"
				"      - Crew\n"),
			legacyClipboardText("    name: Rescuer\n    flags: 0\n    group:\n"
				"      nested: deep\n")
		};

		for (auto const& text : bad)
		{
			AgentClipboardPayload payload;
			std::string diagnostic;
			require(!readAgentClipboardObject(YAML::Load(text)["prometheumClipboard"]["object"],
				payload, diagnostic),
				"A non-name Agent group was accepted: " + text);
			require(!diagnostic.empty(),
				"A non-name Agent group was refused without a reason");
			require(!payload.group.has_value(),
				"A refused Agent group left a value behind");
		}

		// And the refusal stops there: nothing was placed and nothing was
		// committed.
		require(agentCount(*world.building) == 0,
			"A refused clipboard payload placed an Agent");
		require(world.building->getAgentGroupCount() == 0,
			"A refused clipboard payload created an Agent group");
		require(!gBuildingDocumentHistory.canUndo(),
			"A refused clipboard payload committed an undo entry");
	}

	// The destination already defines the name: its own group is the one the
	// pasted Agent joins, and no second group of that name is made.
	void aPasteReusesTheDestinationGroupOfTheSameExactName()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Destination with the group");
		auto const crew = world.building->addAgentGroup("Crew");
		auto const local = createAgent(*world.building, "Local hand", world.room);
		assign(*world.building, local, crew);
		auto const groupsBefore = world.building->getAgentGroupCount();

		// A payload naming "Crew", as a copy from another Building would
		// carry it.
		AgentClipboardPayload payload;
		payload.name = "Transplant";
		payload.group = "Crew";

		auto const placed = place(world.building, payload,
			world.building->getSector(world.corridor), 0, 5.0f);

		require(world.building->getAgentGroup(placed) == crew,
			"The pasted Agent did not join the destination's own group");
		require(world.building->getAgentGroupCount() == groupsBefore,
			"Reusing an Agent group created a second group");
		require(countGroupsNamed(*world.building, "Crew") == 1,
			"The destination ended up with two groups named Crew");
		require(world.building->getAgentGroupMemberCount(crew) == 2,
			"The reused group does not count the pasted Agent");
		require(gBuildingDocumentHistory.undoCount() == 1,
			"A paste is one document edit");
	}

	// Group identity is not name equality across case: "crew" is a different
	// group from "Crew", exactly as the Building's own uniqueness rule says.
	void aPasteMatchesAnAgentGroupNameExactlyAndCaseSensitively()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Case sensitive target");
		auto const crew = world.building->addAgentGroup("Crew");
		auto const groupsBefore = world.building->getAgentGroupCount();

		AgentClipboardPayload payload;
		payload.name = "Night hand";
		payload.group = "crew";

		auto const placed = place(world.building, payload,
			world.building->getSector(world.corridor), 0, 6.0f);

		require(world.building->getAgentGroup(placed) != crew,
			"A pasted Agent joined a group whose name differs by case");
		require(world.building->getAgentGroupCount() == groupsBefore + 1,
			"A case-different Agent group was not created");
		require(countGroupsNamed(*world.building, "crew") == 1,
			"The lower-case Agent group is not the one the paste named");
		require(world.building->getAgentGroupMemberCount(crew) == 0,
			"The existing group took a member the payload never assigned to it");
	}

	// The missing-group case, and the one-edit part of it: the group and the
	// Agent appear together or not at all.
	void aPasteCreatesAMissingAgentGroupAndAssignsInTheSameEdit()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Missing group target");
		auto const groupsBefore = world.building->getAgentGroupCount();
		auto const agentsBefore = agentCount(*world.building);

		AgentClipboardPayload payload;
		payload.name = "Welder";
		payload.flags = 3;
		payload.group = "Welders";

		auto const placed = place(world.building, payload,
			world.building->getSector(world.room), 0, 1.5f);

		auto const created = findGroupNamed(*world.building, "Welders");
		require(!!created, "The paste did not create the missing Agent group");
		require(world.building->getAgentGroupCount() == groupsBefore + 1,
			"Creating a missing Agent group disturbed the other groups");
		require(world.building->getAgentGroup(placed) == created,
			"The pasted Agent was not assigned to the group the paste created");
		require(world.building->getAgentGroupMemberCount(created) == 1,
			"The created group does not count the pasted Agent");
		require(agentCount(*world.building) == agentsBefore + 1,
			"The paste did not add exactly one Agent");
		require(world.building->lookupAgent(placed).entity->getFlags() == 3,
			"The pasted Agent lost the flags its payload carried");
		require(gBuildingDocumentHistory.undoCount() == 1,
			"A paste that creates a group is one document edit, not several");
	}

	// Arming is the half of a paste that happens at the keystroke; it is
	// allowed to hold a payload and nothing else.
	void armingADeferredPlacementWritesNothing()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Arming target");
		auto const agentsBefore = agentCount(*world.building);

		AgentClipboardPayload payload;
		payload.name = "Rescuer";
		payload.group = "Rescue";

		PendingAgentPlacement pending;
		std::string diagnostic;
		require(armAgentPlacement(pending, *world.building, payload,
			world.building->getSector(world.corridor), 0, 2.5f, diagnostic),
			"Arming a valid deferred paste failed: " + diagnostic);
		require(pending.armed(), "Arming a paste left no pending placement");
		require(agentCount(*world.building) == agentsBefore,
			"Arming a paste created an Agent");
		require(world.building->getAgentGroupCount() == 0,
			"Arming a paste created an Agent group");
		require(!gBuildingDocumentHistory.canUndo(), "Arming a paste committed an undo entry");
	}

	// A cancelled fall is a dropped struct: there is nothing left that could
	// write, and nothing to land afterwards either.
	void cancellingADeferredPasteLeavesNothingBehind()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Cancel target");
		auto const agentsBefore = agentCount(*world.building);

		AgentClipboardPayload payload;
		payload.name = "Rescuer";
		payload.group = "Rescue";

		PendingAgentPlacement pending;
		std::string diagnostic;
		require(armAgentPlacement(pending, *world.building, payload,
			world.building->getSector(world.corridor), 0, 2.5f, diagnostic),
			"Arming a deferred paste failed: " + diagnostic);

		pending.cancel();

		require(!pending.armed(), "A cancelled placement is still armed");
		require(agentCount(*world.building) == agentsBefore,
			"Cancelling a paste created an Agent");
		require(world.building->getAgentGroupCount() == 0,
			"Cancelling a paste created an Agent group");
		require(!gBuildingDocumentHistory.canUndo(), "Cancelling a paste committed an undo entry");

		core::AgentId placed{};
		require(!commitPendingAgentPlacement(pending, world.building, placed, diagnostic),
			"A cancelled placement landed anyway");
		require(!placed, "A cancelled placement named an Agent it never made");
		require(agentCount(*world.building) == agentsBefore,
			"Landing a cancelled placement created an Agent");
		require(world.building->getAgentGroupCount() == 0,
			"Landing a cancelled placement created an Agent group");
		require(!gBuildingDocumentHistory.canUndo(), "Landing a cancelled placement committed an undo entry");
	}

	// A placement that cannot be performed is refused whole. The Agent is
	// created first, so the ordinary failure - a Sector that will not take
	// an Agent - never reaches the group creation at all.
	void aFailedPlacementCreatesNoAgentNoGroupAndNoUndoEntry()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Failure target");
		auto const agentsBefore = agentCount(*world.building);

		AgentClipboardPayload payload;
		payload.name = "Rescuer";
		payload.group = "Rescue";

		core::AgentId placed{};
		std::string diagnostic;
		require(!commitAgentPlacement(world.building, payload,
			world.building->getSector(world.background), 0, 1.0f, placed, diagnostic),
			"A placement into a Background was accepted");
		require(!placed, "A refused placement named an Agent it never made");
		require(!diagnostic.empty(), "A refused placement reported no reason");
		require(agentCount(*world.building) == agentsBefore,
			"A failed placement added an Agent anyway");
		require(world.building->getAgentGroupCount() == 0,
			"A failed placement created the Agent group the payload named");
		require(!gBuildingDocumentHistory.canUndo(), "A failed placement committed an undo entry");

		// The same refusal for a placement with nothing to place into.
		resetUndoHistory();
		require(!commitAgentPlacement(world.building, payload, nullptr, 0, 1.0f,
			placed, diagnostic), "A placement with no sector was accepted");
		require(!diagnostic.empty(), "A placement with no sector reported no reason");
		require(!gBuildingDocumentHistory.canUndo(),
			"A placement with no sector committed an undo entry");

		resetUndoHistory();
		require(!commitAgentPlacement(nullptr, payload,
			world.building->getSector(world.corridor), 0, 1.0f, placed, diagnostic),
			"A placement with no Building was accepted");
		require(!diagnostic.empty(), "A placement with no Building reported no reason");
	}

	// An unusable group name is refused whole: not truncated to fit the
	// limit, not quietly turned into "no group", and not created as a
	// partial group on the way to a failure.
	void anInvalidClipboardAgentGroupNameIsRefusedWhole()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Invalid name target");

		std::string const overlong(core::AgentGroup::MaxNameBytes + 1, 'n');
		std::vector<std::pair<std::string, char const*>> const bad = {
			{ "   ", "a blank Agent group name" },
			{ overlong, "an overlong Agent group name" },
		};

		for (auto const& [name, what] : bad)
		{
			AgentClipboardPayload payload;
			payload.name = "Rescuer";
			payload.group = name;

			std::string diagnostic;
			PendingAgentPlacement pending;
			require(!armAgentPlacement(pending, *world.building, payload,
				world.building->getSector(world.corridor), 0, 1.0f, diagnostic),
				std::string("Arming ") + what + " was accepted");
			require(!pending.armed(),
				std::string("Arming ") + what + " left a placement pending");
			require(!diagnostic.empty(),
				std::string("Arming ") + what + " reported no reason");

			core::AgentId placed{};
			require(!commitAgentPlacement(world.building, payload,
				world.building->getSector(world.corridor), 0, 1.0f, placed, diagnostic),
				std::string("Placing ") + what + " was accepted");
			require(!placed, std::string("Placing ") + what + " named an Agent");
			require(!diagnostic.empty(),
				std::string("Placing ") + what + " reported no reason");
		}

		// Nothing was made, at any length: the overlong name was not cut
		// down to the limit and stored, and the blank one was not replaced
		// with a group of no name.
		require(world.building->getAgentGroupCount() == 0,
			"An invalid clipboard Agent group name created a group anyway");
		require(agentCount(*world.building) == 0,
			"A refused Agent group name still placed an Agent");
		require(!gBuildingDocumentHistory.canUndo(),
			"An invalid clipboard Agent group name committed an undo entry");
	}

	// One edit means one undo. The Agent and the group the paste made leave
	// together and come back together, with the assignment intact.
	void undoTakesThePastedAgentAndItsNewGroupTogether()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Undo target");
		auto const agentsBefore = agentCount(*world.building);
		auto const groupsBefore = world.building->getAgentGroupCount();

		AgentClipboardPayload payload;
		payload.name = "Welder";
		payload.group = "Welders";

		auto const placed = place(world.building, payload,
			world.building->getSector(world.room), 0, 1.5f);
		auto const created = findGroupNamed(*world.building, "Welders");
		require(gBuildingDocumentHistory.undoCount() == 1, "The paste committed one undo entry");

		restoreDocument(world.building, false);

		require(agentCount(*world.building) == agentsBefore,
			"Undo left the pasted Agent in the document");
		require(world.building->getAgentGroupCount() == groupsBefore,
			"Undo left the Agent group the paste created");
		require(countGroupsNamed(*world.building, "Welders") == 0,
			"Undo left a group named Welders behind");

		restoreDocument(world.building, true);

		require(agentCount(*world.building) == agentsBefore + 1,
			"Redo did not bring the pasted Agent back");
		require(countGroupsNamed(*world.building, "Welders") == 1,
			"Redo did not bring the created Agent group back");
		auto const restored = findGroupNamed(*world.building, "Welders");
		require(restored == created,
			"Redo restored the Agent group under a different identity");
		require(world.building->getAgentGroup(placed) == restored,
			"Redo restored the Agent and the group but not the assignment");
		require(world.building->getAgentGroupMemberCount(restored) == 1,
			"The restored group does not count the restored Agent");
	}

	// Undoing a paste that reused an existing group leaves the group - it was
	// never the paste's to take - and takes only the assignment with it.
	void undoOfAReusingPasteLeavesTheDestinationGroupAlone()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Reuse undo target");
		auto const crew = world.building->addAgentGroup("Crew");
		auto const local = createAgent(*world.building, "Local hand", world.room);
		assign(*world.building, local, crew);

		AgentClipboardPayload payload;
		payload.name = "Transplant";
		payload.group = "Crew";

		place(world.building, payload, world.building->getSector(world.corridor), 0, 5.0f);
		require(world.building->getAgentGroupMemberCount(crew) == 2,
			"The reused group did not take the pasted Agent");

		restoreDocument(world.building, false);

		require(!!world.building->lookupAgentGroup(crew),
			"Undoing a paste that only reused an Agent group deleted it");
		require(world.building->getAgentGroupMemberCount(crew) == 1,
			"Undoing a paste left its Agent assigned to the reused group");
		require(world.building->getAgentGroup(local) == crew,
			"Undoing a paste disturbed an Agent it never touched");
	}

	// Cutting one member of a group is not a way to delete the group: it
	// stays defined, its other members keep their assignment, and the
	// payload the cut put on the clipboard still names it.
	void cuttingAGroupedAgentLeavesItsSourceGroupDefined()
	{
		resetUndoHistory();
		auto world = buildPasteWorld("Cut target");
		auto const crew = world.building->addAgentGroup("Crew");
		auto const alice = createAgent(*world.building, "Alice", world.room);
		auto const bob = createAgent(*world.building, "Bob", world.room);
		assign(*world.building, alice, crew);
		assign(*world.building, bob, crew);

		// The copy step runs before the removal, exactly as a cut performs
		// it: the payload names the group while the Agent still holds it.
		auto const text = copyText(*world.building, alice, "Alice", true);
		require(text.find("group: Crew") != std::string::npos,
			"A cut payload did not name the Agent's group: " + text);

		std::string diagnostic;
		require(cutAgent(world.building, alice, diagnostic),
			"Cutting a grouped Agent failed: " + diagnostic);

		require(!world.building->lookupAgent(alice),
			"The cut Agent is still in the Building");
		require(!!world.building->lookupAgentGroup(crew),
			"Cutting a grouped Agent deleted its Agent group");
		require(world.building->getAgentGroupMemberCount(crew) == 1,
			"Cutting one member changed how many the group counts");
		require(world.building->getAgentGroup(bob) == crew,
			"Cutting one member disturbed another member's assignment");
		require(agentCount(*world.building) == 1,
			"Cutting one Agent removed more than that Agent");

		// And the payload the cut left behind still pastes into the group it
		// names, in the same Building or any other.
		auto const read = readClipboard(text);
		require(read.cut, "A cut payload read back as a copy");
		require(read.payload.group.has_value() && *read.payload.group == "Crew",
			"A cut payload lost the Agent group name");
	}

	// The whole trip: copy in one Building, paste in another that already
	// defines the same group name. The two Buildings' Agent group IDs are
	// unrelated, and the paste lands on the destination's own.
	void anAgentCopiedBetweenBuildingsJoinsTheDestinationGroup()
	{
		resetUndoHistory();
		auto const source = buildPasteWorld("Source Building");
		auto const destination = buildPasteWorld("Destination Building");

		auto const sourceCrew = source.building->addAgentGroup("Crew");
		auto const alice = createAgent(*source.building, "Alice", source.room);
		assign(*source.building, alice, sourceCrew);

		// The destination defines its own groups first, so its "Crew" is a
		// different AgentGroupId from the source's. Had the clipboard
		// carried the source's ID, the paste would have landed on "Alpha".
		destination.building->addAgentGroup("Alpha");
		destination.building->addAgentGroup("Bravo");
		auto const destinationCrew = destination.building->addAgentGroup("Crew");
		require(destinationCrew != sourceCrew,
			"The fixture gave both Buildings the same Agent group ID, which makes"
			" this check prove nothing");

		auto const text = copyText(*source.building, alice, "Alice copy");
		require(text.find("group: Crew") != std::string::npos,
			"The copied payload did not name the Agent group: " + text);
		auto const read = readClipboard(text);

		auto const placed = place(destination.building, read.payload,
			destination.building->getSector(destination.corridor), 0, 7.0f);

		require(destination.building->getAgentGroup(placed) == destinationCrew,
			"A pasted Agent joined a group by its source Building's ID rather"
			" than the destination's own");
		require(destination.building->getAgentGroupCount() == 3,
			"Pasting into a Building that already had the group added another");
		require(destination.building->getAgentGroupMemberCount(destinationCrew) == 1,
			"The destination's own group did not take the pasted Agent");
		require(source.building->getAgentGroupMemberCount(sourceCrew) == 1,
			"Pasting into another Building disturbed the source group");
	}
}

void runAgentGroupClipboardSmokeChecks()
{
	aCopiedAgentCarriesItsAgentGroupByNameAndNoLocalId();
	aCopiedUngroupedAgentCarriesNoAgentGroup();
	aLegacyPayloadWithoutAnAgentGroupStillPastes();
	aClipboardAgentGroupThatIsNotANameIsRefused();
	aPasteReusesTheDestinationGroupOfTheSameExactName();
	aPasteMatchesAnAgentGroupNameExactlyAndCaseSensitively();
	aPasteCreatesAMissingAgentGroupAndAssignsInTheSameEdit();
	armingADeferredPlacementWritesNothing();
	cancellingADeferredPasteLeavesNothingBehind();
	aFailedPlacementCreatesNoAgentNoGroupAndNoUndoEntry();
	anInvalidClipboardAgentGroupNameIsRefusedWhole();
	undoTakesThePastedAgentAndItsNewGroupTogether();
	undoOfAReusingPasteLeavesTheDestinationGroupAlone();
	cuttingAGroupedAgentLeavesItsSourceGroupDefined();
	anAgentCopiedBetweenBuildingsJoinsTheDestinationGroup();
}
