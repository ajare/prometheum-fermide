// Assigning Agents to Agent groups, for ticket #110.
//
// Everything here crosses the same two seams the group definitions did in
// #109: the public Building authoring API, and a complete Building
// serialize/deserialize round trip. The registries behind the API are never
// inspected, and the YAML is read as a whole document - never asserted
// against incidental formatting.
//
// What gets pinned down:
//
//   every Agent starts with no Agent group, and so does every Agent read from
//   a document that never carried the assignment field
//   assigning and clearing go through Building, and an unknown Agent or an
//   unknown Agent group is refused with a reason and changes nothing
//   the assignment is a reference to stable identity: renaming the group
//   moves the label every assigned Agent shows and leaves the reference alone
//   version 9 carries each assignment by ID through save/load, a missing
//   field loads as no group, and an assignment to a group the file never
//   defined refuses the whole document before any of its Agents is taken in
//   assigning and clearing work while the simulation runs, mark the document
//   modified, and commit exactly one undoable document edit each; a refused
//   operation commits none
//   the real Group cell renders inside a CPU-side ImGui context without
//   leaking a disabled scope, paused or running
//   grouping an Agent changes nothing about how it moves, what its runtime
//   snapshot says, or what events its run publishes

#include <bit>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "core/Agent.h"
#include "core/Building.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/YamlSerializer.h"

#include "AgentGroupAssignmentPanel.h"
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

	// Loads over a Building that already exists, which is how a refused open
	// gets caught out for leftover state.
	void loadInto(core::Building& target, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(target.deserialize(*reader, workData), "The Building did not reload");
	}

	std::string replaceOnce(std::string const& yaml, std::string const& find,
		std::string const& replace)
	{
		auto const found = yaml.find(find);
		require(found != std::string::npos,
			("The serialised Building did not contain \"" + find + "\" to rewrite").c_str());
		return yaml.substr(0, found) + replace + yaml.substr(found + find.size());
	}

	// Drops every Agent assignment line from the document, leaving everything
	// else exactly as it was: the way to ask what a file that never carried an
	// assignment loads as.
	std::string withoutAssignmentFields(std::string const& yaml)
	{
		std::ostringstream out;
		std::istringstream in(yaml);
		std::string line;
		bool removedAny{ false };

		while (std::getline(in, line))
		{
			auto const start = line.find_first_not_of(" \t");
			if (start != std::string::npos && line.compare(start, 6, "group:") == 0)
			{
				removedAny = true;
				continue;
			}
			out << line << "\n";
		}

		require(removedAny,
			"The serialised Building carried no Agent group assignment to strip");
		return out.str();
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

	// A Building with a little world in it, so an Agent is never the only
	// thing the document carries.
	void buildWorld(core::Building& building)
	{
		building.addCorridor(0, 0, 8);
		building.addRoom("Depot", 0, 2, 0, 4, 1);
		building.finishBuild();
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

	void resetUndoHistory()
	{
		gUndoHistory.clear();
		gRedoHistory.clear();
		gCurrentStateId = 0;
		gNextStateId = 1;
		gSavedStateId.reset();
	}

	// ---------------------------------------------------------------- checks

	// No Agent is born with a group, and none is refused a clear on the
	// strength of having none.
	void everyAgentStartsWithNoAgentGroup()
	{
		core::Building building("Assignment defaults", 12, 3);
		buildWorld(building);

		building.addAgentGroup("Crew");
		auto const first = building.createAgent("Alice", 0);
		auto const second = building.createAgent("Bob", 0);

		require(!building.lookupAgent(first).entity->getAgentGroupId(),
			"A newly created Agent came with an Agent group already assigned");
		require(!building.getAgentGroup(first),
			"Building reported an Agent group for a freshly created Agent");
		require(!building.lookupAgent(second).entity->getAgentGroupId(),
			"A second newly created Agent came with an Agent group already assigned");

		// Clearing an Agent that has nothing to clear is a legitimate no-op,
		// not an error: `<none>` is a choice the user can always make.
		std::string diagnostic;
		require(building.canSetAgentGroup(first, {}, &diagnostic),
			("Clearing an Agent with no Agent group was refused: " + diagnostic).c_str());
		require(building.setAgentGroup(first, {}, &diagnostic),
			("Clearing an Agent with no Agent group failed: " + diagnostic).c_str());
		require(!building.lookupAgent(first).entity->getAgentGroupId(),
			"Clearing an Agent that had no Agent group gave it one");
	}

	// The Building is the only way in, and it refuses both halves of a bad
	// assignment without moving anything.
	void anAgentCanBeAssignedAndClearedThroughTheBuilding()
	{
		core::Building building("Assignment API", 12, 3);
		buildWorld(building);

		auto const crew = building.addAgentGroup("Crew");
		auto const nightShift = building.addAgentGroup("Night shift");
		auto const alice = building.createAgent("Alice", 0);

		std::string diagnostic;
		require(building.canSetAgentGroup(alice, crew, &diagnostic),
			("Assigning an Agent to a defined group was refused: " + diagnostic).c_str());
		require(building.setAgentGroup(alice, crew, &diagnostic),
			("Assigning an Agent to a defined group failed: " + diagnostic).c_str());
		require(building.lookupAgent(alice).entity->getAgentGroupId() == crew,
			"An assigned Agent does not report the group it was assigned to");
		require(building.getAgentGroup(alice) == crew,
			"Building.getAgentGroup did not read back the assignment");

		// Re-assigning replaces the previous group rather than adding a second:
		// an Agent belongs to one Agent group, or to none.
		require(building.setAgentGroup(alice, nightShift, &diagnostic),
			("Reassigning an Agent failed: " + diagnostic).c_str());
		require(building.getAgentGroup(alice) == nightShift,
			"Reassigning an Agent left it on its previous group");

		require(building.setAgentGroup(alice, {}, &diagnostic),
			("Clearing an assignment failed: " + diagnostic).c_str());
		require(!building.getAgentGroup(alice),
			"Clearing an assignment left the Agent holding a group");

		// Unknown Agent: refused, with a reason, and nothing else moved.
		require(!building.canSetAgentGroup(core::AgentId{ 424242 }, crew, &diagnostic),
			"Assigning an Agent this Building never issued succeeded");
		require(diagnostic.find("424242") != std::string::npos,
			("The unknown-Agent refusal did not name the Agent: " + diagnostic).c_str());
		require(!building.setAgentGroup(core::AgentId{ 424242 }, crew, &diagnostic),
			"Building.setAgentGroup accepted an Agent it does not own");
		require(!diagnostic.empty(),
			"An unknown Agent was refused without a diagnostic");

		// Unknown group: refused, with the group's ID in the reason.
		require(!building.canSetAgentGroup(alice, core::AgentGroupId{ 999 }, &diagnostic),
			"Assigning an Agent to an Agent group this Building never defined succeeded");
		require(diagnostic.find("999") != std::string::npos,
			("The unknown-group refusal did not name the group: " + diagnostic).c_str());
		require(!building.setAgentGroup(alice, core::AgentGroupId{ 999 }, &diagnostic),
			"Building.setAgentGroup accepted a group it does not own");
		require(!building.getAgentGroup(alice),
			"A refused assignment still changed the Agent");
	}

	// The Agent holds an ID, the Building holds the name, and that is why a
	// rename never has to visit the members.
	void anAssignedAgentFollowsItsGroupRename()
	{
		core::Building building("Assignment rename", 12, 3);
		buildWorld(building);

		auto const crew = building.addAgentGroup("Crew");
		auto const alpha = building.addAgentGroup("Alpha");
		auto const alice = building.createAgent("Alice", 0);
		auto const bob = building.createAgent("Bob", 0);

		std::string diagnostic;
		require(building.setAgentGroup(alice, crew, &diagnostic)
			&& building.setAgentGroup(bob, crew, &diagnostic),
			("Assigning two Agents to one group failed: " + diagnostic).c_str());

		require(building.renameAgentGroup(crew, "Facilities", &diagnostic),
			("Renaming an assigned group was refused: " + diagnostic).c_str());

		require(building.getAgentGroup(alice) == crew
			&& building.getAgentGroup(bob) == crew,
			"Renaming an Agent group broke the assignment it carried");
		require(building.getAgentGroupName(building.getAgentGroup(alice)) == "Facilities",
			"An assigned Agent does not read the group's new name through its ID");
		require(building.getAgentGroupName(building.getAgentGroup(bob)) == "Facilities",
			"A second assigned Agent does not read the group's new name through its ID");
		require(building.getAgentGroupName(alpha) == "Alpha",
			"Renaming one Agent group disturbed another");

		// The panel's own label follows the same way, which is what the user
		// sees in the table without the Agent being touched.
		require(agentGroupAssignmentLabel(building, alice) == "Facilities",
			"The Group cell label did not follow the rename");
		require(agentGroupAssignmentLabel(building, bob) == "Facilities",
			"A second assigned Agent's Group cell label did not follow the rename");
		require(agentGroupAssignmentLabel(building, building.createAgent("Carol", 0)) == "<none>",
			"An Agent with no Agent group does not read as <none>");
	}

	// A world with two groups and three Agents: one per group and one with
	// none, so a round trip has every case to carry.
	struct AssignmentFixture
	{
		std::string yaml;
		core::AgentGroupId crew{};
		core::AgentGroupId nightShift{};
		core::AgentId alice{};
		core::AgentId bob{};
		core::AgentId carol{};
	};

	AssignmentFixture assignmentFixture()
	{
		AssignmentFixture fixture;
		core::Building building("Assignment round trip", 12, 3);
		buildWorld(building);

		fixture.crew = building.addAgentGroup("Crew");
		fixture.nightShift = building.addAgentGroup("Night shift");
		fixture.alice = building.createAgent("Alice", 0);
		fixture.bob = building.createAgent("Bob", 0);
		fixture.carol = building.createAgent("Carol", 0);

		std::string diagnostic;
		require(building.setAgentGroup(fixture.alice, fixture.crew, &diagnostic)
			&& building.setAgentGroup(fixture.bob, fixture.nightShift, &diagnostic),
			("Setting up the assignment round trip failed: " + diagnostic).c_str());

		fixture.yaml = serializeBuilding(building);
		return fixture;
	}

	// Each assignment travels by ID and comes back as the same reference.
	void assignmentsRoundTripThroughSaveAndLoad()
	{
		auto const fixture = assignmentFixture();

		require(fixture.yaml.find("version: 9") != std::string::npos,
			"Agent group assignments were written without a version 9 schema");
		require(fixture.yaml.find("group: 1") != std::string::npos
			&& fixture.yaml.find("group: 2") != std::string::npos,
			"The Agent group assignments were not persisted by ID");

		auto const loaded = loadBuilding(fixture.yaml);
		require(loaded->getAgentGroup(fixture.alice) == fixture.crew,
			"An assigned Agent came back without its Agent group");
		require(loaded->getAgentGroup(fixture.bob) == fixture.nightShift,
			"A second assigned Agent came back without its Agent group");
		require(!loaded->getAgentGroup(fixture.carol),
			"An Agent with no Agent group came back with one");
		require(loaded->getAgentGroupName(loaded->getAgentGroup(fixture.alice)) == "Crew",
			"The reloaded assignment does not name the group it points at");

		// Re-saving what was just loaded produces the same document, so the
		// stored form is canonical and a load cannot drift it.
		require(serializeBuilding(*loaded) == fixture.yaml,
			"Re-saving a reloaded Building produced a different assignment document");

		// And the reloaded Building still assigns, clears and refuses exactly
		// the way the original did.
		std::string diagnostic;
		require(loaded->setAgentGroup(fixture.carol, fixture.crew, &diagnostic),
			("Assigning on a reloaded Building failed: " + diagnostic).c_str());
		require(loaded->getAgentGroup(fixture.carol) == fixture.crew,
			"An assignment made after a load was not kept");
		require(!loaded->setAgentGroup(fixture.alice, core::AgentGroupId{ 7777 }, &diagnostic),
			"A reloaded Building accepted an Agent group it never defined");
		require(loaded->getAgentGroup(fixture.alice) == fixture.crew,
			"A refused assignment on a reloaded Building changed the Agent");
	}

	// A document that never carried the field is not an error and is not a
	// half-truth: every Agent simply has no group.
	void aMissingAssignmentFieldLoadsAsNoGroup()
	{
		auto const fixture = assignmentFixture();
		auto const stripped = withoutAssignmentFields(fixture.yaml);

		auto const loaded = loadBuilding(stripped);
		require(loaded->getAgentGroupCount() == 2,
			"Stripping the assignment fields took the Agent group definitions with them");
		require(!loaded->getAgentGroup(fixture.alice)
			&& !loaded->getAgentGroup(fixture.bob)
			&& !loaded->getAgentGroup(fixture.carol),
			"An Agent whose document carried no assignment field loaded with a group");
		require(agentGroupAssignmentLabel(*loaded, fixture.alice) == "<none>",
			"A legacy Agent with no assignment does not read as <none>");
	}

	// The hazard this guards is a silent downgrade: an assignment to a group
	// that is not in the file could be dropped on the floor and look like a
	// choice the user never made. It is refused instead, and refused before any
	// of the file's Agents is taken in, so the Building is never left holding
	// part of a document it rejected.
	void aDanglingAssignmentRefusesTheFileBeforeAnyAgentIsTakenIn()
	{
		auto const fixture = assignmentFixture();
		// Bob's assignment points at a group the document never defines.
		auto const dangling = replaceOnce(fixture.yaml, "group: 2", "group: 99");

		core::Building target("Dangling target", 12, 3);
		buildWorld(target);
		target.addAgentGroup("Existing");
		auto const ownAgent = target.createAgent("Mine", 0);
		require(ownAgent.value != 0, "The test target could not create its own Agent");

		bool refused = false;
		std::string reason;
		try
		{
			loadInto(target, dangling);
		}
		catch (core::SerializationException const& error)
		{
			refused = true;
			reason = error.what();
		}

		require(refused, "A version-9 Agent assignment to an unknown Agent group was accepted");
		require(reason.find("99") != std::string::npos,
			("The dangling-assignment refusal did not name the group: " + reason).c_str());
		require(reason.find("Bob") != std::string::npos,
			("The dangling-assignment refusal did not name the Agent: " + reason).c_str());

		// Nothing from the refused document was taken in: the target holds no
		// Agent at all, and certainly none pointing at a group it does not own.
		require(allAgents(target).empty(),
			"A refused dangling assignment still let some of the file's Agents in");
	}

	// The editor's own commit path, with the simulation live: one accepted
	// assignment or clearing is one undo entry, one refused operation is none,
	// and neither pauses the world nor touches its topology.
	void assignmentEditsRunAlongsideTheSimulationAndCommitOneUndoEach()
	{
		resetUndoHistory();

		auto const building = std::make_shared<core::Building>("Assignment edits", 12, 3);
		buildWorld(*building);
		require(!building->isSimulationPaused(),
			"The test Building started paused, so it proved nothing about running edits");

		auto const crew = building->addAgentGroup("Crew");
		auto const nightShift = building->addAgentGroup("Night shift");
		auto const alice = building->createAgent("Alice", 0);

		// Let the world actually run, then come back to the document clean so
		// "marked modified" means this operation did it. markSaved() rather than
		// markUnmodified(): the Agents this test created are children of the
		// document, and a dirty Agent keeps isModified() true on its own.
		building->advanceTick();
		building->advanceTick();
		building->markSaved();
		require(!building->isModified(), "The test Building did not come back clean");

		auto const topologyBefore = building->getTopologyGeneration();
		std::string diagnostic;

		require(commitAgentGroupAssignment(building, alice, crew, diagnostic),
			("Assigning through the panel seam failed: " + diagnostic).c_str());
		require(building->getAgentGroup(alice) == crew,
			"The panel seam did not assign the Agent");
		require(building->isModified(),
			"Assigning an Agent to an Agent group did not mark the document modified");
		require(gUndoHistory.size() == 1,
			"Assigning an Agent did not commit exactly one undoable document edit");
		require(gRedoHistory.empty(), "Assigning an Agent produced a redo entry");
		require(!building->isSimulationPaused(),
			"Assigning an Agent paused the simulation");
		require(building->getTopologyGeneration() == topologyBefore,
			"Assigning an Agent rebuilt the traversal topology");

		require(commitAgentGroupAssignment(building, alice, nightShift, diagnostic),
			("Reassigning through the panel seam failed: " + diagnostic).c_str());
		require(gUndoHistory.size() == 2,
			"Reassigning an Agent did not commit exactly one undoable document edit");

		require(commitAgentGroupAssignment(building, alice, {}, diagnostic),
			("Clearing through the panel seam failed: " + diagnostic).c_str());
		require(!building->getAgentGroup(alice),
			"Clearing through the panel seam left the Agent assigned");
		require(gUndoHistory.size() == 3,
			"Clearing an Agent did not commit exactly one undoable document edit");

		// Refused operations leave the history exactly where it was.
		require(!commitAgentGroupAssignment(building, core::AgentId{ 4242 }, crew, diagnostic),
			"Assigning an unknown Agent succeeded through the panel seam");
		require(gUndoHistory.size() == 3,
			"A refused assignment committed an undo entry");
		require(!diagnostic.empty(),
			"An assignment refused through the panel seam failed without a diagnostic");

		require(!commitAgentGroupAssignment(building, alice, core::AgentGroupId{ 31337 }, diagnostic),
			"Assigning to an unknown Agent group succeeded through the panel seam");
		require(gUndoHistory.size() == 3,
			"An assignment to an unknown group committed an undo entry");
		require(!building->getAgentGroup(alice),
			"A refused assignment changed the live Agent");

		// Undo is a snapshot restore, so the entries themselves are the
		// history: the newest holds the state with the assignment still on,
		// the oldest the state before the first assignment was made.
		require(gUndoHistory.size() == 3, "The undo stack is not the three edits made");
		auto const beforeClear = loadBuilding(gUndoHistory.back().yaml);
		require(beforeClear->getAgentGroup(alice) == nightShift,
			"The newest undo snapshot did not hold the state before the clearing");
		auto const beforeFirst = loadBuilding(gUndoHistory.front().yaml);
		require(!beforeFirst->getAgentGroup(alice),
			"The oldest undo snapshot already carried the first assignment");

		// And the live Building is where the redo would take it.
		require(!building->getAgentGroup(alice),
			"The live Building did not hold the cleared assignment");
	}

	// The label the cell shows, checked against the states it can be in. The
	// choice order itself is the Building's creation order, which #109 pins
	// down; the cell renders straight off it, `<none>` always leading.
	void theGroupCellLabelShowsTheAssignment()
	{
		core::Building building("Assignment label", 12, 3);
		buildWorld(building);

		auto const crew = building.addAgentGroup("Crew");
		auto const alice = building.createAgent("Alice", 0);

		require(agentGroupAssignmentLabel(building, alice) == "<none>",
			"An unassigned Group cell does not read <none>");
		std::string diagnostic;
		require(building.setAgentGroup(alice, crew, &diagnostic),
			("Assigning for the label check failed: " + diagnostic).c_str());
		require(agentGroupAssignmentLabel(building, alice) == "Crew",
			"An assigned Group cell does not show the group's name");
		require(building.renameAgentGroup(crew, "Response team", &diagnostic),
			("Renaming for the label check failed: " + diagnostic).c_str());
		require(agentGroupAssignmentLabel(building, alice) == "Response team",
			"An assigned Group cell does not follow a rename");
		require(building.setAgentGroup(alice, {}, &diagnostic)
			&& agentGroupAssignmentLabel(building, alice) == "<none>",
			"A cleared Group cell does not read <none> again");
	}

	// The real cell, rendered for real, inside a table like the one the Agents
	// section builds. What matters is that it leaves no ImGui state behind: a
	// leaked disabled scope once dimmed the rest of the editor for every frame.
	void theGroupCellRendersWithoutLeakingImGuiState()
	{
		ImGuiGuard guard;

		auto const shared = std::make_shared<core::Building>("Group cell render", 12, 3);
		buildWorld(*shared);
		auto const crew = shared->addAgentGroup("Crew");
		// A second group so the cell has a list to render, not just the choice
		// it already shows.
		shared->addAgentGroup("Night shift");
		auto const alice = shared->createAgent("Alice", 0);
		std::string diagnostic;
		require(shared->setAgentGroup(alice, crew, &diagnostic),
			("Assigning for the render check failed: " + diagnostic).c_str());

		for (bool const paused : { true, false })
		{
			if (paused) shared->pauseSimulation();
			else if (shared->isSimulationPaused()) shared->resumeSimulation();

			ImGui::NewFrame();
			ImGui::Begin("Building");

			ImGuiTableFlags const flags =
				ImGuiTableFlags_SizingStretchSame |
				ImGuiTableFlags_BordersOuter |
				ImGuiTableFlags_BordersV;
			require(ImGui::BeginTable("Agents", 5, flags),
				"The test Agents table could not be opened");
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Group");
			ImGui::TableSetupColumn("Sector");
			ImGui::TableSetupColumn("State");
			ImGui::TableSetupColumn("Path");
			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::PushID((void const*)shared->lookupAgent(alice).entity);
			ImGui::TableSetColumnIndex(1);

			auto const depthOnEntry = GImGui->DisabledStackSize;
			auto const flagsOnEntry = GImGui->CurrentItemFlags;
			auto const alphaOnEntry = GImGui->Style.Alpha;

			renderAgentGroupAssignmentCell(shared, alice);

			require(GImGui->DisabledStackSize == depthOnEntry,
				"The Group cell left a disabled scope open");
			require(GImGui->CurrentItemFlags == flagsOnEntry,
				"The Group cell changed the current item flags");
			// Compared as bits, not as floats: the question is whether the value
			// is exactly the one stored, which is what "unchanged" means here
			// and what -Wfloat-equal objects to otherwise.
			require(std::bit_cast<uint32_t>(GImGui->Style.Alpha)
				== std::bit_cast<uint32_t>(alphaOnEntry),
				"The Group cell changed the global alpha");
			require(shared->getAgentGroup(alice) == crew,
				"Merely rendering the Group cell changed the assignment");

			ImGui::PopID();
			ImGui::EndTable();
			ImGui::End();

			// The frame has to complete, which is where ImGui's own end-frame
			// checks would fire on an unbalanced window.
			ImGui::Render();
		}
	}

	// A deterministic little walk: one Agent crossing a Corridor to a Marker.
	// The trace is the tick, every Agent snapshot, and every event the run
	// published, rendered as text so two runs can be compared as wholes.
	std::string walkTheCorridor(bool withGroups)
	{
		core::Building building("Assignment walk", 12, 3);
		auto const corridor = building.addCorridor(0, 0, 8);
		uint32_t destinationIdentifier{ 0x41475231u };
		building.addSectorMarker(corridor, 0, 7.5f, &destinationIdentifier);
		building.finishBuild();

		core::AgentGroupId crew{};
		if (withGroups)
		{
			crew = building.addAgentGroup("Crew");
			building.addAgentGroup("Night shift");
		}

		auto const walker = building.createAgent("Walker", corridor, 0, 0.5f);
		if (withGroups)
		{
			std::string diagnostic;
			require(building.setAgentGroup(walker, crew, &diagnostic),
				("Grouping the walking Agent failed: " + diagnostic).c_str());
		}

		auto* agent = building.lookupAgent(walker).entity;
		auto const destination = building.getGraph()->getVertexByIdentifier(destinationIdentifier);
		require(destination != nullptr, "The walk destination vertex is missing");
		auto path = building.getGraph()->calculatePath(agent, destination);
		require(path && !path->nodes.empty(), "The walk route could not be calculated");
		agent->setPath(std::move(path), true);

		auto const startGlobal = agent->getGlobalPosition();
		for (uint32_t tick = 0; tick < 90; ++tick) building.advanceTick();

		require(agent->getGlobalPosition().distanceTo(startGlobal) > 0.01f,
			"The test Agent did not actually move, so the comparison proved nothing");
		require(agent->getAgentGroupId() == (withGroups ? crew : core::AgentGroupId{}),
			"Grouping changed what the Agent itself reports about its group");

		std::ostringstream out;
		out << std::fixed << std::setprecision(6);

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

	// Grouping is editor metadata. Two Buildings that differ only in whether
	// their Agent has an Agent group must move, snapshot, and event alike.
	void groupingAnAgentChangesNothingInTheSimulation()
	{
		auto const ungrouped = walkTheCorridor(false);
		auto const grouped = walkTheCorridor(true);

		require(!ungrouped.empty() && !grouped.empty(),
			"The movement traces came back empty, so the comparison proved nothing");
		require(ungrouped == grouped,
			"Assigning an Agent to an Agent group changed its movement, runtime snapshot, "
			"or events:\n" + ungrouped + "\nvs\n" + grouped);
	}
}

void runAgentGroupAssignmentSmokeChecks()
{
	everyAgentStartsWithNoAgentGroup();
	anAgentCanBeAssignedAndClearedThroughTheBuilding();
	anAssignedAgentFollowsItsGroupRename();
	assignmentsRoundTripThroughSaveAndLoad();
	aMissingAssignmentFieldLoadsAsNoGroup();
	aDanglingAssignmentRefusesTheFileBeforeAnyAgentIsTakenIn();
	assignmentEditsRunAlongsideTheSimulationAndCommitOneUndoEach();
	theGroupCellLabelShowsTheAssignment();
	theGroupCellRendersWithoutLeakingImGuiState();
	groupingAnAgentChangesNothingInTheSimulation();
}
