// Deleting Agent groups, for ticket #112.
//
// The hazard this whole file circles is a stale reference: a group removed
// while Agents still carry its ID. Such a Building cannot even be saved and
// read back - the file format refuses an assignment to a group the document
// never defines - so the deletion has to take the assignments with it, in the
// same step, and must be undoable as one step afterwards.
//
// What gets pinned down:
//
//   Building::deleteAgentGroup is the sole mutation boundary: every Agent
//   carrying the deleted AgentGroupId is returned to no Agent group before
//   the group itself is removed, and the whole thing is judged before a
//   single field is written
//   an unknown or empty AgentGroupId is refused with a diagnostic that names
//   it, and the Building is left exactly as it was found
//   a deleted AgentGroupId is never issued again, so an old reference can
//   never come back pointing at a different group
//   the saved document after a deletion carries neither the group nor any
//   assignment to it, and a reload agrees
//   an empty group deletes on the spot with no confirmation asked; an
//   occupied group arms a confirmation whose text carries the authoritative
//   member count read off the Building
//   cancelling changes no group, no assignment, no count, no dirty state and
//   no undo history - the underlying no-op contract, checked as a whole
//   confirming performs the group removal and every assignment clearing as
//   exactly one document edit, and every former member's Group cell reads
//   `<none>`
//   the confirmation really reaches the screen as a modal, and a dismissal
//   that was never answered is a cancel rather than a half-armed request
//   deletion is available with the simulation running, leaves the runtime
//   snapshot and events alone at the tick it happens, and does not disturb
//   the movement of the Agents that were its members
//   undo restores the group's identity, creation-order position, name,
//   assignments and count; redo removes them again

#include <cstdint>
#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "core/Agent.h"
#include "core/Building.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/Vertex.h"
#include "core/YamlSerializer.h"

#include "AgentGroupAssignmentPanel.h"
#include "AgentGroupsPanel.h"
#include "DocumentEdit.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeBuilding(core::Building& building)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	// A whole-document load, the way the editor opens a file.
	std::shared_ptr<core::Building> loadBuilding(std::string const& yaml)
	{
		auto loaded = std::make_shared<core::Building>("Loaded Building", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(reader != nullptr, "The serialised Building could not be read back");
		require(loaded->deserialize(*reader, workData), "The Building did not reload");
		return loaded;
	}

	// The lines inside one top-level collection - "agentGroups", "agents" -
	// whose `key` field is exactly this Agent group ID. The collection and the
	// key are both named by the caller because the two collections use
	// different ones for the same number: a group carries itself under `id:`,
	// an Agent carries its group under `group:`. Matching either key anywhere
	// would let an Agent's own `id: 2` stand in for the group's `id: 2`, and
	// matching a substring would let "2" hide inside "cellsWide: 12".
	std::vector<std::string> linesCarrying(std::string const& yaml,
		std::string const& collection, std::string const& key, uint64_t id)
	{
		std::vector<std::string> found;
		std::istringstream in(yaml);
		std::string line;
		bool inside{ false };

		while (std::getline(in, line))
		{
			auto const start = line.find_first_not_of(" \t");
			if (start == std::string::npos) continue;

			if (start == 0)
			{
				// A top-level key: inside this collection from its own line
				// until the next top-level key that is not it.
				inside = line.compare(0, collection.size() + 1, collection + ":") == 0;
				continue;
			}
			if (!inside) continue;

			// An array item's first key sits on the same line as its dash.
			auto content = start;
			if (line[content] == '-')
			{
				++content;
				while (content < line.size()
					&& (line[content] == ' ' || line[content] == '\t'))
				{
					++content;
				}
			}

			if (line.compare(content, key.size(), key) != 0) continue;
			auto const rest = line.substr(content + key.size());
			auto const valueStart = rest.find_first_not_of(" \t");
			if (valueStart == std::string::npos) continue;
			if (rest.substr(valueStart) == std::to_string(id)) found.push_back(line);
		}
		return found;
	}

	std::vector<core::Agent const*> allAgents(core::Building const& building)
	{
		std::vector<core::Agent const*> agents;
		for (uint32_t layer = 0; layer < building.getLayerCount(); ++layer)
		{
			for (auto const& sector : building.getSectors(layer))
			{
				if (!sector) continue;
				for (auto* agent : sector->getAgents())
				{
					if (agent) agents.push_back(agent);
				}
			}
		}
		return agents;
	}

	// Every group the Building reports, with its count, as one comparable
	// line. Used to ask whether anything other than the deletion moved.
	std::string groupSummary(core::Building const& building)
	{
		std::string out;
		for (auto const id : building.getAgentGroupIds())
		{
			out += std::to_string(id.value) + ':' + building.getAgentGroupName(id) + '='
				+ std::to_string(building.getAgentGroupMemberCount(id)) + ';';
		}
		return out;
	}

	// Every Agent the Building owns, with the group it carries, as a sorted
	// list of comparable entries. Sorted because the order Agents sit in a
	// Sector is not part of what an assignment is, and a reload is free to
	// put two Agents in the same room in a different order.
	std::vector<std::string> assignmentEntries(core::Building const& building)
	{
		std::vector<std::string> entries;
		for (auto const* agent : allAgents(building))
		{
			auto const group = agent->getAgentGroupId();
			entries.push_back(std::string(agent->getName()) + "->"
				+ (group ? std::to_string(group.value) : std::string("<none>")));
		}
		std::sort(entries.begin(), entries.end());
		return entries;
	}

	std::string assignmentSummary(core::Building const& building)
	{
		std::string out;
		for (auto const& entry : assignmentEntries(building)) out += entry + ";";
		return out;
	}

	// Nothing an Agent carries may name a group this Building does not own.
	// The one state a save would refuse to read back, and the one state this
	// ticket exists to make unreachable.
	void requireNoDanglingAssignment(core::Building const& building,
		std::string const& context)
	{
		for (auto const* agent : allAgents(building))
		{
			auto const group = agent->getAgentGroupId();
			if (!group) continue;
			require(static_cast<bool>(building.lookupAgentGroup(group)),
				("An Agent is left naming an Agent group this Building does not own: "
					+ std::string(agent->getName()) + " -> " + std::to_string(group.value)
					+ " (" + context + ")").c_str());
		}
	}

	struct ImGuiGuard
	{
		ImGuiGuard()
		{
			ImGui::CreateContext();
			auto& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	// Called from inside a frame, in the same ID scope the panel uses: is the
	// deletion confirmation popup open? Read off ImGui's own popup stack
	// rather than a window's Active flag, because a window begun on the frame
	// it closes is still flagged active until the frame after.
	bool confirmationPopupOpen()
	{
		return ImGui::IsPopupOpen("Delete Agent group?");
	}

	void resetUndoHistory()
	{
		gBuildingDocumentHistory.clear();
	}

	// ---------------------------------------------------------------- world

	// Two Layers, two Locations on the front one and two behind, so a group's
	// members are spread across the Building rather than sitting side by side.
	struct DeleteWorld
	{
		uint32_t frontCorridor{ 0 };
		uint32_t frontRoom{ 0 };
		uint32_t backRoom{ 0 };
		uint32_t backCorridor{ 0 };
	};

	DeleteWorld buildDeleteWorld(core::Building& building)
	{
		DeleteWorld world;
		world.frontCorridor = building.addCorridor(0, 0, 12);
		world.frontRoom = building.addRoom("Front room", 0, 2, 0, 6, 1);
		world.backRoom = building.addRoom("Back room", 1, 0, 0, 6, 1);
		world.backCorridor = building.addCorridor(1u, 2u, 0u, 12u, 1u);
		building.finishBuild();
		return world;
	}

	void assign(core::Building& building, core::AgentId agent, core::AgentGroupId group)
	{
		std::string diagnostic;
		require(building.setAgentGroup(agent, group, &diagnostic),
			("Assigning an Agent for a deletion check failed: " + diagnostic).c_str());
	}

	// The fixture every check starts from: three groups, the middle one
	// occupied by four Agents spread over both Layers, the outer two empty.
	struct DeleteFixture
	{
		core::AgentGroupId alpha{};
		core::AgentGroupId crew{};
		core::AgentGroupId delta{};
		std::vector<core::AgentId> members;
		core::AgentId unassigned{};
	};

	DeleteFixture buildFixture(core::Building& building)
	{
		DeleteFixture fixture;
		auto const world = buildDeleteWorld(building);

		fixture.alpha = building.addAgentGroup("Alpha");
		fixture.crew = building.addAgentGroup("Crew");
		fixture.delta = building.addAgentGroup("Delta");

		fixture.members.push_back(building.createAgent("Front corridor hand",
			world.frontCorridor, 0, 1.5f));
		fixture.members.push_back(building.createAgent("Front desk",
			world.frontRoom, 0, 1.0f));
		fixture.members.push_back(building.createAgent("Back office",
			world.backRoom, 0, 2.0f));
		fixture.members.push_back(building.createAgent("Back corridor hand",
			world.backCorridor, 0, 3.0f));
		fixture.unassigned = building.createAgent("Freelance", world.frontCorridor, 0, 5.0f);

		for (auto const member : fixture.members) assign(building, member, fixture.crew);

		return fixture;
	}

	// The editor's own undo and redo, the same shape as UI.cpp's
	// restoreDocumentSnapshot(): the live state crosses to the other stack and
	// the newest snapshot on the source stack becomes the live Building.
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
			if (!loaded->deserialize(*reader, workData)) return false;
			loaded->markModified();
			return true;
		};
		auto const restored = redo
			? gBuildingDocumentHistory.redo(current, restore)
			: gBuildingDocumentHistory.undo(current, restore);
		require(restored, redo ? "There is no redo entry to restore"
			: "There is no undo entry to restore");
		building = std::move(loaded);
		// The same reset the editor performs when the document underneath the
		// panels is replaced.
		resetAgentGroupsPanelState();
	}

	// ---------------------------------------------------------------- checks

	// An empty group is the simple case, and it has to be simple: the group
	// goes, and nothing else moves.
	void anEmptyGroupDeletesAndLeavesEveryOtherGroupAlone()
	{
		core::Building building("Empty delete", 12, 3);
		auto const fixture = buildFixture(building);
		auto const before = groupSummary(building);
		require(before.find("Alpha") != std::string::npos
			&& before.find("Delta") != std::string::npos,
			"The fixture did not start with the two empty groups: " + before);

		std::string diagnostic;
		require(building.canDeleteAgentGroup(fixture.alpha, &diagnostic),
			("Deleting an empty Agent group was refused: " + diagnostic).c_str());
		require(building.deleteAgentGroup(fixture.alpha, &diagnostic),
			("Deleting an empty Agent group failed: " + diagnostic).c_str());

		require(building.getAgentGroupCount() == 2,
			"The Building still reports three Agent groups after deleting one: "
			+ groupSummary(building));
		require(!building.lookupAgentGroup(fixture.alpha),
			"The deleted Agent group is still resolvable through the Building");
		require(groupSummary(building) == "2:Crew=4;3:Delta=0;",
			"Deleting an empty Agent group disturbed the others: " + groupSummary(building));
		requireNoDanglingAssignment(building, "after an empty group delete");
	}

	// The occupied case, which is the one that can go wrong. Every member is
	// read back off the Agent itself - not off the group, which is gone - and
	// every one of them is on no group afterwards, whichever Layer it sits on.
	void deletingAnOccupiedGroupReturnsEveryMemberToNoGroup()
	{
		core::Building building("Occupied delete", 12, 3);
		auto const fixture = buildFixture(building);
		require(building.getAgentGroupMemberCount(fixture.crew) == 4,
			"The fixture's occupied group does not start with four members");

		std::string diagnostic;
		require(building.deleteAgentGroup(fixture.crew, &diagnostic),
			("Deleting an occupied Agent group failed: " + diagnostic).c_str());

		require(!building.lookupAgentGroup(fixture.crew),
			"The occupied Agent group was not removed");
		require(building.getAgentGroupCount() == 2,
			"The Building did not lose exactly the deleted Agent group: "
			+ groupSummary(building));

		for (auto const member : fixture.members)
		{
			require(!building.getAgentGroup(member),
				("A former member of the deleted Agent group is still assigned to a group: "
					+ std::to_string(member.value)).c_str());
			require(!building.lookupAgent(member).entity->getAgentGroupId(),
				"The Agent itself still carries the deleted AgentGroupId");
			require(agentGroupAssignmentLabel(building, member) == "<none>",
				"A former member's Group cell does not read <none> after the delete");
		}

		// The Agent that was never a member is untouched by all of this.
		require(!building.getAgentGroup(fixture.unassigned),
			"An Agent that was never a member came out of the delete assigned");
		require(groupSummary(building) == "1:Alpha=0;3:Delta=0;",
			"The surviving groups are not the two empty ones: " + groupSummary(building));
		requireNoDanglingAssignment(building, "after an occupied group delete");
	}

	// An ID this Building never issued is refused, and the refusal says which
	// ID. Nothing is written on the way out, so "atomically" has a concrete
	// meaning here: the whole summary is what it was before the call.
	void anUnknownGroupIdIsRefusedAtomicallyWithADiagnostic()
	{
		core::Building building("Unknown delete", 12, 3);
		auto const fixture = buildFixture(building);
		auto const before = groupSummary(building);
		auto const beforeAssignments = assignmentSummary(building);
		auto const beforeIds = building.getAgentGroupIds();

		std::string diagnostic;
		require(!building.canDeleteAgentGroup(core::AgentGroupId{ 4242 }, &diagnostic),
			"Deleting an Agent group this Building never issued succeeded");
		require(diagnostic.find("4242") != std::string::npos,
			("The unknown-group refusal did not name the group: " + diagnostic).c_str());
		require(!building.deleteAgentGroup(core::AgentGroupId{ 4242 }, &diagnostic),
			"Building.deleteAgentGroup accepted a group it does not own");
		require(diagnostic.find("4242") != std::string::npos,
			("The refused delete did not name the unknown group: " + diagnostic).c_str());

		// The null handle names no group either, so there is nothing for it
		// to delete; refusing it keeps a success meaning a deletion happened.
		require(!building.canDeleteAgentGroup(core::AgentGroupId{}, &diagnostic),
			"Deleting the empty AgentGroupId was treated as a deletion");
		require(!building.deleteAgentGroup(core::AgentGroupId{}, &diagnostic),
			"Building.deleteAgentGroup accepted the empty AgentGroupId");
		require(!diagnostic.empty(),
			"The empty-AgentGroupId refusal came back without a reason");

		require(groupSummary(building) == before,
			"A refused delete changed the groups: " + groupSummary(building));
		require(assignmentSummary(building) == beforeAssignments,
			"A refused delete cleared an assignment it had no business touching");
		require(building.getAgentGroupIds() == beforeIds,
			"A refused delete disturbed the Agent group list");
	}

	// A deleted AgentGroupId is never handed out again, so a reference that
	// somehow survived could never silently come to mean a different group.
	void aDeletedAgentGroupIdIsNeverIssuedAgain()
	{
		core::Building building("Id reuse", 12, 3);
		auto const fixture = buildFixture(building);

		std::string diagnostic;
		require(building.deleteAgentGroup(fixture.crew, &diagnostic),
			("Deleting the occupied Agent group failed: " + diagnostic).c_str());

		auto const replacement = building.addAgentGroup("Replacement");
		require(replacement != fixture.crew,
			"A new Agent group was issued the deleted group's AgentGroupId");
		require(replacement.value > fixture.crew.value,
			"The new Agent group's AgentGroupId did not come after the deleted one");
		requireNoDanglingAssignment(building, "after reissuing Agent group IDs");
	}

	// Grouping is editor metadata, not topology: deleting a group marks the
	// document dirty and leaves the traversal graph and the running world
	// exactly as they were.
	void aDeletionMarksTheDocumentAndLeavesTheTopologyAlone()
	{
		auto building = std::make_shared<core::Building>("Delete dirty state", 12, 3);
		auto const fixture = buildFixture(*building);
		building->advanceTick();
		building->markSaved();
		require(!building->isModified(), "The test Building did not come back clean");

		auto const topologyBefore = building->getTopologyGeneration();
		std::string diagnostic;
		require(commitAgentGroupDelete(building, fixture.crew, diagnostic),
			("Committing an Agent group delete failed: " + diagnostic).c_str());

		require(building->isModified(),
			"Deleting an Agent group did not mark the document modified");
		require(building->getTopologyGeneration() == topologyBefore,
			"Deleting an Agent group rebuilt the traversal topology");
		require(!building->isSimulationPaused(),
			"Deleting an Agent group paused the simulation");
	}

	// The document after a deletion carries neither the group nor any
	// assignment to it, and a reload agrees with the live Building about
	// every Agent.
	void aDeletedGroupStaysDeletedThroughASaveAndReopen()
	{
		core::Building building("Delete round trip", 12, 3);
		auto const fixture = buildFixture(building);

		// The helper has to find the ID while it is really there, or the
		// "still carries it" check below would pass on a helper that matched
		// nothing at all: one definition line in agentGroups, one assignment
		// line in agents per member.
		auto const presentDefinitions = linesCarrying(serializeBuilding(building),
			"agentGroups", "id:", fixture.crew.value);
		auto const presentAssignments = linesCarrying(serializeBuilding(building),
			"agents", "group:", fixture.crew.value);
		require(presentDefinitions.size() == 1,
			("The stale-reference scan did not find the live Agent group definition: found "
				+ std::to_string(presentDefinitions.size())).c_str());
		require(presentAssignments.size() == fixture.members.size(),
			("The stale-reference scan did not find every live assignment: found "
				+ std::to_string(presentAssignments.size())).c_str());

		std::string diagnostic;
		require(building.deleteAgentGroup(fixture.crew, &diagnostic),
			("Deleting the occupied Agent group failed: " + diagnostic).c_str());

		auto const yaml = serializeBuilding(building);
		require(yaml.find("name: Crew") == std::string::npos,
			"The saved document still defines the deleted Agent group");
		auto const staleDefinitions = linesCarrying(yaml, "agentGroups", "id:",
			fixture.crew.value);
		auto const staleAssignments = linesCarrying(yaml, "agents", "group:",
			fixture.crew.value);
		require(staleDefinitions.empty(),
			("The saved document still defines the deleted AgentGroupId "
				+ std::to_string(fixture.crew.value) + ": "
				+ (staleDefinitions.empty() ? std::string("<none>")
					: staleDefinitions.front())).c_str());
		require(staleAssignments.empty(),
			("The saved document still assigns an Agent to the deleted AgentGroupId "
				+ std::to_string(fixture.crew.value) + ": "
				+ (staleAssignments.empty() ? std::string("<none>")
					: staleAssignments.front())).c_str());

		auto const loaded = loadBuilding(yaml);
		require(loaded->getAgentGroupCount() == 2,
			"The reopened Building did not come back with two Agent groups: "
			+ groupSummary(*loaded));
		require(!loaded->lookupAgentGroup(fixture.crew),
			"The reopened Building can still resolve the deleted AgentGroupId");
		require(groupSummary(*loaded) == "1:Alpha=0;3:Delta=0;",
			"The reopened Building's groups are not what was saved: "
			+ groupSummary(*loaded));
		for (auto const member : fixture.members)
		{
			require(!loaded->getAgentGroup(member),
				("A former member came back assigned to the deleted Agent group: "
					+ std::to_string(member.value)).c_str());
			require(agentGroupAssignmentLabel(*loaded, member) == "<none>",
				"A reopened former member's Group cell does not read <none>");
		}
		requireNoDanglingAssignment(*loaded, "after reopening a document with a delete in it");

		// Re-saving what was just loaded produces the same document, so the
		// deletion is canonical rather than a live-only correction.
		require(serializeBuilding(*loaded) == yaml,
			"Re-saving the reopened Building produced a different document");
	}

	// The confirmation split, read straight off the Building: what needs
	// asking about, and what the asking says.
	void onlyAnOccupiedGroupNeedsConfirmingAndSaysHowMany()
	{
		core::Building building("Delete confirmation text", 12, 3);
		auto const fixture = buildFixture(building);

		require(!agentGroupDeleteRequiresConfirmation(building, fixture.alpha),
			"An empty Agent group was judged to need a confirmation");
		require(!agentGroupDeleteRequiresConfirmation(building, fixture.delta),
			"A second empty Agent group was judged to need a confirmation");
		require(agentGroupDeleteRequiresConfirmation(building, fixture.crew),
			"An occupied Agent group was judged not to need a confirmation");

		auto const text = agentGroupDeleteConfirmationText(building, fixture.crew);
		require(text.find("Crew") != std::string::npos,
			("The confirmation does not name the Agent group: " + text).c_str());
		require(text.find("4") != std::string::npos,
			("The confirmation does not state the member count: " + text).c_str());
		require(text.find("no Agent group") != std::string::npos,
			("The confirmation does not say where the Agents go: " + text).c_str());

		// One member reads as one, not as some plural that quietly rounds the
		// impact up or down.
		auto const single = building.addAgentGroup("Single hand");
		assign(building, building.createAgent("Lone worker", 0, 0, 2.5f), single);
		require(agentGroupDeleteRequiresConfirmation(building, single),
			"A one-member Agent group was judged not to need a confirmation");
		auto const singleText = agentGroupDeleteConfirmationText(building, single);
		require(singleText.find("1 Agent ") != std::string::npos,
			("A one-member confirmation is not worded as one Agent: " + singleText).c_str());
		require(singleText.find("2 Agent") == std::string::npos,
			("A one-member confirmation overstates the impact: " + singleText).c_str());

		// The count in the text is the Building's, not a remembered one: move
		// a member in and the next text says more.
		assign(building, building.createAgent("Late arrival", 0, 0, 3.5f), single);
		require(agentGroupDeleteConfirmationText(building, single).find("2 Agent")
			!= std::string::npos,
			("The confirmation text did not follow a new assignment: "
				+ agentGroupDeleteConfirmationText(building, single)).c_str());
	}

	// An empty group through the panel's own entry point: deleted on the spot,
	// one document edit, nothing armed and no confirmation asked.
	void anEmptyGroupDeletesOnTheSpotThroughThePanelSeam()
	{
		resetUndoHistory();
		resetAgentGroupsPanelState();

		ImGuiGuard guard;
		ImGui::NewFrame();
		ImGui::Begin("Building");

		auto building = std::make_shared<core::Building>("Empty delete seam", 12, 3);
		auto const fixture = buildFixture(*building);

		requestAgentGroupDelete(building, fixture.alpha);

		require(!building->lookupAgentGroup(fixture.alpha),
			"An empty Agent group asked for through the panel seam was not deleted");
		require(!agentGroupDeletePending(),
			"Deleting an empty Agent group left a confirmation armed");
		require(gBuildingDocumentHistory.undoCount() == 1,
			"Deleting an empty Agent group did not commit exactly one document edit");
		require(!gBuildingDocumentHistory.canRedo(),
			"Deleting an empty Agent group produced a redo entry");
		require(groupSummary(*building) == "2:Crew=4;3:Delta=0;",
			"Deleting an empty Agent group disturbed the others: " + groupSummary(*building));

		// Rendered after the request: an empty group's delete raised no
		// confirmation, so there is nothing on the screen to be answered.
		renderAgentGroupsPanel(building);
		require(!confirmationPopupOpen(),
			"Deleting an empty Agent group put a confirmation on the screen");
		ImGui::End();
		ImGui::Render();
	}

	// An occupied group asked for through the panel seam changes nothing at
	// all until the answer arrives.
	void anOccupiedGroupWaitsForAnAnswerAndHasChangedNothingYet()
	{
		resetUndoHistory();
		resetAgentGroupsPanelState();

		auto building = std::make_shared<core::Building>("Occupied delete pending", 12, 3);
		auto const fixture = buildFixture(*building);
		building->markSaved();

		auto const groupsBefore = groupSummary(*building);
		auto const assignmentsBefore = assignmentSummary(*building);
		auto const historyBefore = gBuildingDocumentHistory.undoCount();

		requestAgentGroupDelete(building, fixture.crew);

		core::AgentGroupId pending{};
		uint32_t armedCount{ 0 };
		require(agentGroupDeletePending(&pending, &armedCount),
			"Deleting an occupied Agent group did not arm a confirmation");
		require(pending == fixture.crew,
			"The armed confirmation is for a different Agent group than was asked for");
		require(armedCount == 4,
			"The armed confirmation does not carry the Building's member count: "
			+ std::to_string(armedCount));
		require(static_cast<bool>(building->lookupAgentGroup(fixture.crew)),
			"An occupied Agent group was deleted before the user answered");
		require(groupSummary(*building) == groupsBefore,
			"Arming a confirmation changed the groups: " + groupSummary(*building));
		require(assignmentSummary(*building) == assignmentsBefore,
			"Arming a confirmation changed an assignment");
		require(building->getAgentGroupMemberCount(fixture.crew) == 4,
			"Arming a confirmation changed the member count");
		require(!building->isModified(),
			"Arming a confirmation marked the document modified");
		require(gBuildingDocumentHistory.undoCount() == historyBefore,
			"Arming a confirmation committed a document edit");
		require(!gBuildingDocumentHistory.canRedo(),
			"Arming a confirmation produced a redo entry");
	}

	// The no-op contract behind Cancel: not merely "the group is still there"
	// but nothing else moved either - no assignment, no count, no dirty flag,
	// no history entry, and nothing left armed to delete later.
	void cancellingChangesNoGroupAssignmentCountDirtyStateOrHistory()
	{
		resetUndoHistory();
		resetAgentGroupsPanelState();

		auto building = std::make_shared<core::Building>("Delete cancel", 12, 3);
		auto const fixture = buildFixture(*building);
		building->markSaved();

		requestAgentGroupDelete(building, fixture.crew);
		require(agentGroupDeletePending(), "The confirmation was not armed to be cancelled");

		cancelPendingAgentGroupDelete();

		require(!agentGroupDeletePending(),
			"A cancelled Agent group deletion is still awaiting an answer");
		require(building->getAgentGroupCount() == 3,
			"Cancelling changed the number of Agent groups: " + groupSummary(*building));
		require(static_cast<bool>(building->lookupAgentGroup(fixture.crew)),
			"Cancelling removed the Agent group that was only asked about");
		require(building->getAgentGroupMemberCount(fixture.crew) == 4,
			"Cancelling changed the member count");
		require(building->getAgentGroupName(fixture.crew) == "Crew",
			"Cancelling changed the Agent group's own identity");
		for (auto const member : fixture.members)
		{
			require(building->getAgentGroup(member) == fixture.crew,
				("Cancelling cleared a member's assignment: "
					+ std::to_string(member.value)).c_str());
		}
		require(!building->isModified(),
			"Cancelling an Agent group deletion marked the document modified");
		require(!gBuildingDocumentHistory.canUndo(),
			"Cancelling an Agent group deletion committed an undo entry");
		require(!gBuildingDocumentHistory.canRedo(),
			"Cancelling an Agent group deletion committed a redo entry");
		require(gBuildingDocumentHistory.currentStateId() == 0,
			"Cancelling an Agent group deletion moved the document's state id");

		// And the panel is clean enough to ask again, with the same answer.
		requestAgentGroupDelete(building, fixture.crew);
		require(agentGroupDeletePending(),
			"A second Agent group delete could not be armed after a cancel");
		cancelPendingAgentGroupDelete();
		require(!gBuildingDocumentHistory.canUndo(),
			"A second cancelled Agent group deletion committed an undo entry");
	}

	// Confirming is one document edit, not two: the group and every
	// assignment cleared on its way out are inside the same commit, so no
	// undo can ever land between them.
	void confirmingDeletesTheGroupAndItsAssignmentsAsOneEdit()
	{
		resetUndoHistory();
		resetAgentGroupsPanelState();

		auto building = std::make_shared<core::Building>("Delete confirm", 12, 3);
		auto const fixture = buildFixture(*building);
		building->markSaved();

		requestAgentGroupDelete(building, fixture.crew);

		std::string diagnostic;
		require(confirmPendingAgentGroupDelete(building, diagnostic),
			("Confirming an Agent group deletion failed: " + diagnostic).c_str());
		require(!agentGroupDeletePending(),
			"The confirmation stayed armed after it was answered with a delete");
		require(!building->lookupAgentGroup(fixture.crew),
			"The confirmed Agent group is still in the Building");
		require(gBuildingDocumentHistory.undoCount() == 1,
			"A confirmed Agent group deletion was not exactly one document edit");
		require(!gBuildingDocumentHistory.canRedo(),
			"A confirmed Agent group deletion produced a redo entry");
		require(building->isModified(),
			"A confirmed Agent group deletion did not mark the document modified");

		for (auto const member : fixture.members)
		{
			require(!building->getAgentGroup(member),
				("A former member is still assigned after a confirmed delete: "
					+ std::to_string(member.value)).c_str());
			require(agentGroupAssignmentLabel(*building, member) == "<none>",
				"A former member's Group cell did not become <none>");
		}
		require(groupSummary(*building) == "1:Alpha=0;3:Delta=0;",
			"The surviving groups are not the two empty ones: " + groupSummary(*building));
		requireNoDanglingAssignment(*building, "after a confirmed delete");

		// The newest - and only - undo snapshot is the state with the group
		// and its four members still in place, which is what makes the whole
		// deletion one step to step back.
		auto const before = loadBuilding(gBuildingDocumentHistory.undoEntries().back().yaml);
		require(before->getAgentGroupCount() == 3,
			"The undo snapshot did not hold the state before the deletion: "
			+ groupSummary(*before));
		require(before->getAgentGroupMemberCount(fixture.crew) == 4,
			"The undo snapshot did not hold every member of the deleted group");
	}

	// Answering a confirmation that was never armed cannot delete anything,
	// and says so.
	void confirmingWithNothingArmedDeletesNothing()
	{
		resetUndoHistory();
		resetAgentGroupsPanelState();

		auto building = std::make_shared<core::Building>("Delete with nothing armed", 12, 3);
		auto const fixture = buildFixture(*building);
		auto const before = groupSummary(*building);

		std::string diagnostic;
		require(!confirmPendingAgentGroupDelete(building, diagnostic),
			"A confirmation with nothing armed reported a deletion");
		require(!diagnostic.empty(),
			"A confirmation with nothing armed failed without a reason");
		require(groupSummary(*building) == before,
			"A confirmation with nothing armed changed the groups: "
			+ groupSummary(*building));
		require(!gBuildingDocumentHistory.canUndo(),
			"A confirmation with nothing armed committed an undo entry");
	}

	// A refused delete is refused all the way: no group gone, no assignment
	// cleared, no history entry, and the diagnostic says why.
	void aRefusedDeleteCommitsNothingThroughThePanelSeam()
	{
		resetUndoHistory();
		resetAgentGroupsPanelState();

		auto building = std::make_shared<core::Building>("Refused delete", 12, 3);
		auto const fixture = buildFixture(*building);
		building->markSaved();
		auto const before = groupSummary(*building);

		std::string diagnostic;
		require(!commitAgentGroupDelete(building, core::AgentGroupId{ 31337 }, diagnostic),
			"Committing a delete for an unknown Agent group succeeded");
		require(diagnostic.find("31337") != std::string::npos,
			("The refused delete did not name the unknown Agent group: " + diagnostic).c_str());
		require(!commitAgentGroupDelete(building, core::AgentGroupId{}, diagnostic),
			"Committing a delete for the empty AgentGroupId succeeded");
		require(groupSummary(*building) == before,
			"A refused delete changed the groups: " + groupSummary(*building));
		require(!gBuildingDocumentHistory.canUndo(),
			"A refused delete committed an undo entry");
		require(!building->isModified(),
			"A refused delete marked the document modified");

		// The same refusal through the armed path: a confirmation for a group
		// that is no longer there cannot delete anything.
		requestAgentGroupDelete(building, fixture.crew);
		require(agentGroupDeletePending(), "The confirmation was not armed");
		// Take the group out from under the armed confirmation, the way a
		// document replace would.
		resetAgentGroupsPanelState();
		require(!agentGroupDeletePending(),
			"Replacing the document left a confirmation armed");
		require(!confirmPendingAgentGroupDelete(building, diagnostic),
			"Confirming after the request was dropped reported a deletion");
		require(groupSummary(*building) == before,
			"A dropped delete request changed the groups: " + groupSummary(*building));
		require(!gBuildingDocumentHistory.canUndo(),
			"A dropped delete request committed an undo entry");
	}

	// The confirmation on the screen. The modal really opens, really states
	// the count, and really disappears when it is answered - all of it inside
	// a CPU-side ImGui context, with no ImGui state left behind.
	void theConfirmationReachesTheScreenAsAModal()
	{
		resetUndoHistory();
		resetAgentGroupsPanelState();

		ImGuiGuard guard;

		auto building = std::make_shared<core::Building>("Delete modal", 12, 3);
		auto const fixture = buildFixture(*building);

		// One real frame over the real panel, reporting whether the
		// confirmation was still open at the end of it.
		auto renderOneFrame = [&building]() -> bool
		{
			ImGui::NewFrame();
			ImGui::Begin("Building");
			renderAgentGroupsPanel(building);
			bool const open = confirmationPopupOpen();
			ImGui::End();
			ImGui::Render();
			return open;
		};

		// Nothing armed: no modal on screen.
		require(!renderOneFrame(),
			"A deletion confirmation was on the screen with nothing armed");

		requestAgentGroupDelete(building, fixture.crew);

		// The frame that opens it.
		ImGui::NewFrame();
		ImGui::Begin("Building");

		auto const depthOnEntry = GImGui->DisabledStackSize;
		auto const flagsOnEntry = GImGui->CurrentItemFlags;

		renderAgentGroupsPanel(building);

		require(GImGui->DisabledStackSize == depthOnEntry,
			"The deletion confirmation left a disabled scope open");
		require(GImGui->CurrentItemFlags == flagsOnEntry,
			"The deletion confirmation changed the current item flags");
		// The Building and its Agents are untouched while the question is up.
		require(building->getAgentGroupCount() == 3,
			"The confirmation being on screen changed the Agent groups");
		require(building->getAgentGroupMemberCount(fixture.crew) == 4,
			"The confirmation being on screen changed the member count");
		require(confirmationPopupOpen(),
			"The confirmation is not reported as open while it is on the screen");
		ImGui::End();
		ImGui::Render();

		auto* const opened = ImGui::FindWindowByName("Delete Agent group?");
		require(opened != nullptr, "The deletion confirmation never reached the screen");
		require(opened->Active, "The deletion confirmation window is not active");

		// A frame later it has really been drawn, with content in its draw
		// buffer rather than an empty shell.
		ImGui::NewFrame();
		ImGui::Begin("Building");
		renderAgentGroupsPanel(building);
		ImGui::End();
		ImGui::Render();

		require(opened->WasActive, "The deletion confirmation was never drawn");
		require(opened->DrawList->CmdBuffer.size() > 1,
			"The deletion confirmation drew nothing but an empty window");
		require(building->getAgentGroupCount() == 3,
			"A second frame of the confirmation changed the Agent groups");

		// Answer it, and the modal goes with the answer.
		std::string diagnostic;
		require(confirmPendingAgentGroupDelete(building, diagnostic),
			("Confirming the on-screen deletion failed: " + diagnostic).c_str());

		require(!renderOneFrame(),
			"The confirmation stayed on the screen after it was answered with a delete");
		require(!agentGroupDeletePending(),
			"The confirmation stayed armed after the delete was answered");
		require(building->getAgentGroupCount() == 2,
			"The confirmed deletion did not reach the Building: "
			+ groupSummary(*building));
		requireNoDanglingAssignment(*building, "after answering the on-screen confirmation");
	}

	// Every group's row carries its own Delete control, rendered through the
	// same cell function the table uses, and none of it leaks ImGui state.
	void everyGroupRowCarriesItsOwnDeleteControl()
	{
		ImGuiGuard guard;

		auto building = std::make_shared<core::Building>("Delete column", 12, 3);
		buildFixture(*building);

		ImGui::NewFrame();
		ImGui::Begin("Building");

		ImGuiTableFlags const flags =
			ImGuiTableFlags_SizingStretchSame |
			ImGuiTableFlags_Resizable |
			ImGuiTableFlags_BordersOuter |
			ImGuiTableFlags_BordersV;
		require(ImGui::BeginTable("AgentGroups", 3, flags),
			"The test Groups table could not be opened");
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Agents");
		ImGui::TableSetupColumn("Delete");
		ImGui::TableHeadersRow();

		std::vector<ImGuiID> deleteControlIds;
		for (auto const id : building->getAgentGroupIds())
		{
			ImGui::TableNextRow();
			ImGui::PushID(id.value);
			ImGui::TableSetColumnIndex(2);

			auto const depthOnEntry = GImGui->DisabledStackSize;
			auto const flagsOnEntry = GImGui->CurrentItemFlags;

			renderAgentGroupDeleteCell(building, id);

			require(GImGui->LastItemData.ID != 0,
				("A group row rendered no Delete control: "
					+ std::to_string(id.value)).c_str());
			require(GImGui->DisabledStackSize == depthOnEntry,
				"The Delete cell left a disabled scope open");
			require(GImGui->CurrentItemFlags == flagsOnEntry,
				"The Delete cell changed the current item flags");
			deleteControlIds.push_back(GImGui->LastItemData.ID);
			ImGui::PopID();
		}
		ImGui::EndTable();
		ImGui::End();
		ImGui::Render();

		require(deleteControlIds.size() == 3,
			"The fixture did not render one Delete control per Agent group");
		for (size_t i = 0; i < deleteControlIds.size(); ++i)
		{
			for (size_t j = i + 1; j < deleteControlIds.size(); ++j)
			{
				require(deleteControlIds[i] != deleteControlIds[j],
					"Two Agent group rows share one Delete control identity");
			}
		}
		// Rendering the controls asked for nothing: the groups are exactly
		// what they were before the frame.
		require(groupSummary(*building) == "1:Alpha=0;2:Crew=4;3:Delta=0;",
			"Rendering the Delete controls changed the groups: " + groupSummary(*building));
	}

	// The whole panel, with the Delete column in it, rendered for real with
	// the simulation both paused and running, and nothing left behind.
	void theGroupsWithDeleteRenderWithoutLeakingImGuiState()
	{
		ImGuiGuard guard;

		auto const shared = std::make_shared<core::Building>("Delete panel", 12, 3);
		buildFixture(*shared);

		for (bool const paused : { true, false })
		{
			if (paused) shared->pauseSimulation();
			else if (shared->isSimulationPaused()) shared->resumeSimulation();

			ImGui::NewFrame();
			ImGui::Begin("Building");

			auto const depthOnEntry = GImGui->DisabledStackSize;
			auto const flagsOnEntry = GImGui->CurrentItemFlags;
			auto const groupsBefore = groupSummary(*shared);

			renderAgentGroupsPanel(shared);

			require(GImGui->DisabledStackSize == depthOnEntry,
				"The Agent groups panel left a disabled scope open");
			require(GImGui->CurrentItemFlags == flagsOnEntry,
				"The Agent groups panel changed the current item flags");
			require(groupSummary(*shared) == groupsBefore,
				"Rendering the panel with its Delete column changed the groups: "
				+ groupSummary(*shared));

			ImGui::End();
			ImGui::Render();
		}
	}

	// A deterministic walk whose trace is the tick, the topology generation,
	// every Agent's position and state, and every event the run published.
	// Two runs that differ only in whether the middle of the walk deleted the
	// Agents' group must produce the same trace, which is what "deletion does
	// not alter Agent movement or other simulation state" means concretely.
	std::string walkWithOptionalMidRunDelete(bool deleteMidRun)
	{
		core::Building building("Delete walk", 12, 3);
		auto const corridor = building.addCorridor(0, 0, 12);
		uint32_t destinationIdentifier{ 0x44454c31u };
		building.addSectorMarker(corridor, 0, 11.5f, &destinationIdentifier);
		building.finishBuild();

		auto const crew = building.addAgentGroup("Crew");
		auto const walker = building.createAgent("Walker", corridor, 0, 0.5f);
		auto const companion = building.createAgent("Companion", corridor, 0, 1.5f);
		assign(building, walker, crew);
		assign(building, companion, crew);

		auto* agent = building.lookupAgent(walker).entity;
		auto const destination = building.getGraph()->getVertexByIdentifier(destinationIdentifier);
		require(destination != nullptr, "The walk destination vertex is missing");
		auto path = building.getGraph()->calculatePath(agent, destination);
		require(path && !path->nodes.empty(), "The walk route could not be calculated");
		agent->setPath(std::move(path), true);

		for (uint32_t tick = 0; tick < 30; ++tick) building.advanceTick();

		auto const* walkingAgent = building.lookupAgent(walker).entity;
		auto const positionBeforeDelete = walkingAgent->getGlobalPosition();
		require(!building.isSimulationPaused(),
			"The test Building was paused before the delete, so running was never tested");

		if (deleteMidRun)
		{
			std::string diagnostic;
			require(building.deleteAgentGroup(crew, &diagnostic),
				("Deleting an Agent group mid-run failed: " + diagnostic).c_str());
			require(!building.isSimulationPaused(),
				"Deleting an Agent group paused a running simulation");
		}

		for (uint32_t tick = 0; tick < 90; ++tick) building.advanceTick();

		require(building.lookupAgent(walker).entity->getGlobalPosition()
			.distanceTo(positionBeforeDelete) > 0.01f,
			"The walking Agent did not move after the delete, so the comparison proved nothing");

		std::ostringstream out;
		out.precision(6);
		out << std::fixed;

		auto const snapshot = building.getSimulationSnapshot();
		out << "tick=" << snapshot.tick
			<< " paused=" << snapshot.paused
			<< " topology=" << snapshot.topologyGeneration << "\n";
		for (auto const& entry : snapshot.agents)
		{
			out << "agent " << entry.id.value << ' ' << entry.name
				<< " sector=" << entry.sectorId.value
				<< " local=" << entry.localPosition.x << ',' << entry.localPosition.y
				<< " global=" << entry.globalPosition.x << ',' << entry.globalPosition.y
				<< " state=" << static_cast<int>(entry.state)
				<< " hasPath=" << entry.hasPath
				<< " node=" << entry.targetPathNode << '/' << entry.pathNodeCount
				<< " loco=" << entry.hasLocomotionTask
				<< " request=" << entry.traversalRequest.value
				<< " permit=" << entry.traversalPermit.value
				<< " interaction=" << entry.interactionRequest.value << "\n";
		}
		for (auto const& event : building.consumeSimulationEvents())
		{
			out << "event " << event.sequence << ':' << event.tick
				<< ':' << static_cast<int>(event.type)
				<< ':' << static_cast<int>(event.phase) << "\n";
		}
		return out.str();
	}

	// Deleting while the world runs changes nothing about the world. The
	// Agents that were the group's members carry on walking exactly as if the
	// group had never existed, because grouping was never part of their run.
	void deletingWhileTheSimulationRunsLeavesTheRunAlone()
	{
		auto const withoutDelete = walkWithOptionalMidRunDelete(false);
		auto const withDelete = walkWithOptionalMidRunDelete(true);

		require(!withoutDelete.empty() && !withDelete.empty(),
			"The movement traces came back empty, so the comparison proved nothing");
		require(withoutDelete == withDelete,
			"Deleting an Agent group while the simulation ran changed Agent movement, "
			"the runtime snapshot, or the published events:\n"
			+ withoutDelete + "\nvs\n" + withDelete);
	}

	// The snapshot at the instant of the deletion, and the snapshot a moment
	// later, are the same document: the deletion wrote nothing into the
	// runtime state it shares with the simulation.
	void theRuntimeSnapshotIsTheSameAcrossTheDeletion()
	{
		core::Building building("Snapshot across delete", 12, 3);
		auto const fixture = buildFixture(building);
		for (uint32_t tick = 0; tick < 12; ++tick) building.advanceTick();

		auto const render = [](core::SimulationSnapshot const& snapshot)
		{
			std::ostringstream out;
			out.precision(6);
			out << std::fixed;
			out << "tick=" << snapshot.tick
				<< " paused=" << snapshot.paused
				<< " topology=" << snapshot.topologyGeneration << "\n";
			for (auto const& entry : snapshot.agents)
			{
				out << entry.id.value << ' ' << entry.name
					<< " sector=" << entry.sectorId.value
					<< " global=" << entry.globalPosition.x << ',' << entry.globalPosition.y
					<< " state=" << static_cast<int>(entry.state) << "\n";
			}
			return out.str();
		};

		auto const before = render(building.getSimulationSnapshot());

		std::string diagnostic;
		require(building.deleteAgentGroup(fixture.crew, &diagnostic),
			("Deleting the occupied Agent group failed: " + diagnostic).c_str());

		auto const after = render(building.getSimulationSnapshot());
		require(before == after,
			"The runtime snapshot changed across an Agent group deletion:\n"
			+ before + "\nvs\n" + after);
	}

	// Undo brings back everything the deletion took: the same AgentGroupId,
	// the same place in the creation order, the same name, the same Agents
	// assigned, and therefore the same count. Redo takes all of it back
	// again.
	void undoRestoresTheGroupCompletelyAndRedoRemovesItAgain()
	{
		resetUndoHistory();
		resetAgentGroupsPanelState();

		auto building = std::make_shared<core::Building>("Delete undo", 12, 3);
		auto const fixture = buildFixture(*building);
		auto const idsBefore = building->getAgentGroupIds();
		auto const groupsBefore = groupSummary(*building);
		auto const assignmentsBefore = assignmentSummary(*building);
		require(idsBefore.size() == 3, "The fixture did not start with three Agent groups");

		std::string diagnostic;
		require(commitAgentGroupDelete(building, fixture.crew, diagnostic),
			("Committing the Agent group delete to undo failed: " + diagnostic).c_str());
		require(gBuildingDocumentHistory.undoCount() == 1,
			"The delete is not one undo entry, so undo cannot restore one step");
		auto const postDeleteYaml = serializeBuilding(*building);

		restoreDocument(building, false);

		require(building->getAgentGroupIds() == idsBefore,
			"Undo did not restore the Agent groups in their creation-order positions: "
			+ groupSummary(*building));
		require(static_cast<bool>(building->lookupAgentGroup(fixture.crew)),
			"Undo did not bring back the deleted Agent group under its own AgentGroupId");
		require(building->getAgentGroupName(fixture.crew) == "Crew",
			"Undo restored the Agent group under a different name");
		require(groupSummary(*building) == groupsBefore,
			"Undo did not restore the groups and their counts: " + groupSummary(*building));
		require(assignmentSummary(*building) == assignmentsBefore,
			"Undo did not restore the assignments the deletion cleared: "
			+ assignmentSummary(*building) + " vs " + assignmentsBefore);
		require(building->getAgentGroupMemberCount(fixture.crew) == 4,
			"Undo did not restore the deleted group's count");
		for (auto const member : fixture.members)
		{
			require(building->getAgentGroup(member) == fixture.crew,
				("A former member was not restored to the deleted Agent group: "
					+ std::to_string(member.value)).c_str());
			require(agentGroupAssignmentLabel(*building, member) == "Crew",
				"A restored member's Group cell does not read the group's name again");
		}
		requireNoDanglingAssignment(*building, "after undoing a delete");

		// Redo removes them again, as one step, with nothing left behind.
		restoreDocument(building, true);

		require(!building->lookupAgentGroup(fixture.crew),
			"Redo did not remove the Agent group again");
		require(building->getAgentGroupIds().size() == 2,
			"Redo left the wrong number of Agent groups: " + groupSummary(*building));
		require(groupSummary(*building) == "1:Alpha=0;3:Delta=0;",
			"Redo did not restore the surviving groups to their post-delete state: "
			+ groupSummary(*building));
		for (auto const member : fixture.members)
		{
			require(!building->getAgentGroup(member),
				("Redo left a former member assigned: "
					+ std::to_string(member.value)).c_str());
			require(agentGroupAssignmentLabel(*building, member) == "<none>",
				"A re-deleted member's Group cell does not read <none>");
		}
		requireNoDanglingAssignment(*building, "after redoing a delete");

		// And the document the redo restored is byte for byte the document the
		// deletion left behind, so the round trip is a round trip and not a
		// near miss.
		require(serializeBuilding(*building) == postDeleteYaml,
			"The document restored by redo is not the document the deletion left");
	}
}

void runAgentGroupDeleteSmokeChecks()
{
	resetAgentGroupsPanelState();
	anEmptyGroupDeletesAndLeavesEveryOtherGroupAlone();
	deletingAnOccupiedGroupReturnsEveryMemberToNoGroup();
	anUnknownGroupIdIsRefusedAtomicallyWithADiagnostic();
	aDeletedAgentGroupIdIsNeverIssuedAgain();
	aDeletionMarksTheDocumentAndLeavesTheTopologyAlone();
	aDeletedGroupStaysDeletedThroughASaveAndReopen();
	onlyAnOccupiedGroupNeedsConfirmingAndSaysHowMany();
	anEmptyGroupDeletesOnTheSpotThroughThePanelSeam();
	anOccupiedGroupWaitsForAnAnswerAndHasChangedNothingYet();
	cancellingChangesNoGroupAssignmentCountDirtyStateOrHistory();
	confirmingDeletesTheGroupAndItsAssignmentsAsOneEdit();
	confirmingWithNothingArmedDeletesNothing();
	aRefusedDeleteCommitsNothingThroughThePanelSeam();
	theConfirmationReachesTheScreenAsAModal();
	everyGroupRowCarriesItsOwnDeleteControl();
	theGroupsWithDeleteRenderWithoutLeakingImGuiState();
	deletingWhileTheSimulationRunsLeavesTheRunAlone();
	theRuntimeSnapshotIsTheSameAcrossTheDeletion();
	undoRestoresTheGroupCompletelyAndRedoRemovesItAgain();
	resetAgentGroupsPanelState();
}
