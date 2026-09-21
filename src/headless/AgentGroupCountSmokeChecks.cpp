// Live Agent group membership counts, for ticket #111.
//
// A count is asked through the public Building API and answered from the
// Building's own authoritative state: the Agents themselves, each carrying
// the Agent group ID it was assigned. Nothing here reads a counter, because
// there is none - not on the group, not in the panel, not in the file. That
// is the whole design, and most of what these checks pin down.
//
// What gets pinned down:
//
//   a count covers every Building-owned Agent carrying the group's ID,
//   whichever Layer and Sector it sits in and whichever movement state it is
//   in - idle, walking, or held waiting at a Door it cannot open
//   ungrouped Agents and the members of other groups are never counted, and
//   an empty group counts zero rather than going missing
//   a count is current the moment an assignment is made: assigning,
//   reassigning and clearing each move it by exactly one, with no refresh
//   step for a caller to remember and nothing to make the number settle
//   renaming a group leaves its count alone, because the count follows the
//   ID and the name is only a label read back through the Building
//   removing an Agent takes it out of its group's count, so a count cannot
//   outlive a member the Building no longer has
//   the serialized document carries group definitions and per-Agent
//   assignments only: no count-shaped field exists anywhere in them, so no
//   written-down number can go stale, and a round trip rebuilds every count
//   from the memberships that came back
//   an undo snapshot restores the memberships and the counts with them, and
//   carries no count data of its own either
//   the real Groups table declares the Agents column and renders inside a
//   CPU-side ImGui context without leaking a disabled scope, paused or
//   running

#include <bit>
#include <cctype>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "core/Agent.h"
#include "core/Building.h"
#include "core/Edge.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/Vertex.h"
#include "core/YamlSerializer.h"

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

	// Loads over a Building that already exists, which is how the editor's
	// undo restores a snapshot: the snapshot's document replaces the live one.
	void restoreInto(core::Building& target, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(target.deserialize(*reader, workData), "The Building did not reload");
	}

	// Every Agent the Building owns, read off the spatial index, so a check
	// can cross-read a count against the Agents it claims to be counting.
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

	// The count every group reports, as one comparable line per group. The
	// way to ask whether anything other than an assignment moved a number.
	std::string allCounts(core::Building const& building)
	{
		std::string out;
		for (auto const id : building.getAgentGroupIds())
		{
			out += building.getAgentGroupName(id) + "="
				+ std::to_string(building.getAgentGroupMemberCount(id)) + ";";
		}
		return out;
	}

	uint32_t countByScanningAgents(core::Building const& building, core::AgentGroupId group)
	{
		uint32_t count{ 0 };
		for (auto const* agent : allAgents(building))
		{
			if (agent->getAgentGroupId() == group) ++count;
		}
		return count;
	}

	// The key names used inside one top-level YAML collection - "agentGroups",
	// "agents" - as a whole document is written. A count that had been written
	// down would show up here; the point of the check is that none is.
	std::vector<std::string> keysInCollection(std::string const& yaml,
		std::string const& collection)
	{
		std::vector<std::string> keys;

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

			// An array item's first key sits on the same line as its "-", so
			// the dash is stepped over rather than the whole line skipped.
			auto content = start;
			if (line[content] == '-')
			{
				++content;
				while (content < line.size()
					&& (line[content] == ' ' || line[content] == '\t'))
				{
					++content;
				}
				if (content >= line.size()) continue;
			}

			auto const colon = line.find(':', content);
			if (colon == std::string::npos) continue;
			keys.push_back(line.substr(content, colon - content));
		}
		return keys;
	}

	std::string lowerCase(std::string const& value)
	{
		std::string out;
		out.reserve(value.size());
		for (auto const c : value)
			out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		return out;
	}

	// Nothing count-shaped may appear in the part of the document that carries
	// groups and Agents. A stored total is a second source of truth, and a
	// second source of truth is one that can be wrong.
	void requireNoCountShapedKeys(std::string const& yaml, std::string const& collection)
	{
		auto const keys = keysInCollection(yaml, collection);
		require(!keys.empty(),
			("The serialised document carried no \"" + collection
				+ "\" keys to inspect").c_str());

		for (auto const& key : keys)
		{
			auto const name = lowerCase(key);
			bool const countShaped = name.find("count") != std::string::npos
				|| name.find("member") != std::string::npos
				|| name.find("total") != std::string::npos
				|| name == "size";
			require(!countShaped,
				("The serialised \"" + collection + "\" collection carries a count-shaped key \""
					+ key + "\", which would be a second source of truth for membership").c_str());
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

	void resetUndoHistory()
	{
		gBuildingDocumentHistory.clear();
	}

	// A two-Storey world with two Locations on each Layer, so a group's
	// members are genuinely spread out and a count has to reach all of them.
	struct CountingWorld
	{
		uint32_t frontCorridor{ 0 };
		uint32_t frontRoom{ 0 };
		uint32_t backRoom{ 0 };
		uint32_t backCorridor{ 0 };
	};

	CountingWorld buildCountingWorld(core::Building& building)
	{
		CountingWorld world;
		world.frontCorridor = building.addCorridor(0, 0, 12);
		world.frontRoom = building.addRoom("Front room", 0, 2, 0, 6, 1);
		world.backRoom = building.addRoom("Back room", 1, 0, 0, 6, 1);
		world.backCorridor = building.addCorridor(1u, 2u, 0u, 12u, 1u);
		building.finishBuild();
		return world;
	}

	// Assignment goes through the Building and the count is read straight
	// back: no refresh, no notification, no second call to make it correct.
	void assign(core::Building& building, core::AgentId agent, core::AgentGroupId group)
	{
		std::string diagnostic;
		require(building.setAgentGroup(agent, group, &diagnostic),
			("Assigning an Agent for a count check failed: " + diagnostic).c_str());
	}

	// A two-node path over one edge, the shape the editor gives an
	// explicitly-directed short move. Used here to send an Agent at a Door it
	// is not allowed to open.
	std::shared_ptr<core::Path> twoNodePath(std::shared_ptr<const core::Vertex> source,
		std::shared_ptr<const core::Vertex> destination, std::shared_ptr<const core::Edge> edge)
	{
		auto path = std::make_shared<core::Path>();
		path->nodes.push_back({ nullptr, std::move(source), 0.0f });
		path->nodes.push_back({ std::move(edge), std::move(destination), 1.0f });
		return path;
	}

	// ---------------------------------------------------------------- checks

	// A count is not a per-Layer or per-Sector figure. One call covers the
	// whole Building, and members scattered over Layers and Locations all
	// land in the same number.
	void aCountCoversEveryLayerAndSector()
	{
		core::Building building("Count spread", 12, 3);
		auto const world = buildCountingWorld(building);

		auto const crew = building.addAgentGroup("Crew");
		auto const nightShift = building.addAgentGroup("Night shift");

		// Three Crew members: two on the front Layer in two different
		// Locations, one on the Layer behind.
		auto const frontCorridorAgent = building.createAgent("Corridor hand",
			world.frontCorridor, 0, 1.5f);
		auto const frontRoomAgent = building.createAgent("Front desk",
			world.frontRoom, 0, 1.0f);
		auto const backRoomAgent = building.createAgent("Back office",
			world.backRoom, 0, 2.0f);
		// And two Agents that belong to nobody's count but one each.
		auto const otherGroupAgent = building.createAgent("Night porter",
			world.backCorridor, 0, 3.0f);
		auto const ungroupedAgent = building.createAgent("Passing through",
			world.frontCorridor, 0, 5.0f);

		assign(building, frontCorridorAgent, crew);
		assign(building, frontRoomAgent, crew);
		assign(building, backRoomAgent, crew);
		assign(building, otherGroupAgent, nightShift);

		require(building.getAgentGroupMemberCount(crew) == 3,
			("A group's count did not reach its Agents across Layers and Sectors: "
				+ allCounts(building)).c_str());
		require(building.getAgentGroupMemberCount(nightShift) == 1,
			("The second group's count did not stay on its own member: "
				+ allCounts(building)).c_str());

		// Each count agrees with the Agents that actually carry the ID.
		require(building.getAgentGroupMemberCount(crew)
			== countByScanningAgents(building, crew),
			"A count disagrees with the Agents carrying the group's ID");
		require(building.getAgentGroupMemberCount(nightShift)
			== countByScanningAgents(building, nightShift),
			"A second count disagrees with the Agents carrying the group's ID");

		// The ungrouped Agent is in neither count: it has no Agent group to
		// be counted under, and no group counts it.
		require(!building.getAgentGroup(ungroupedAgent),
			"The ungrouped test Agent picked up an Agent group");
		require(allAgents(building).size() == 5,
			"The test world did not place every Agent the count is meant to cover");
	}

	// An empty group is a group that counts zero, not a group with no number.
	// A freshly defined group and a group whose last member has just gone both
	// read as zero, and the column shows it.
	void anEmptyGroupCountsZero()
	{
		core::Building building("Empty group", 12, 3);
		auto const world = buildCountingWorld(building);

		auto const crew = building.addAgentGroup("Crew");
		require(building.getAgentGroupMemberCount(crew) == 0,
			"A newly defined Agent group did not count zero members");
		require(agentGroupMemberCountLabel(building, crew) == "0",
			"The Agents column did not show zero for an empty Agent group");

		auto const agent = building.createAgent("Only member", world.frontCorridor, 0, 1.0f);
		assign(building, agent, crew);
		require(building.getAgentGroupMemberCount(crew) == 1,
			"The first member of an empty group was not counted");

		assign(building, agent, {});
		require(building.getAgentGroupMemberCount(crew) == 0,
			"Clearing the last member left the group counting something");
		require(agentGroupMemberCountLabel(building, crew) == "0",
			"The Agents column did not return to zero once the group was emptied");
	}

	// Assigning, reassigning and clearing each move the numbers by exactly
	// one, and do it before anything else is asked.
	void countsTrackAssignReassignAndClearImmediately()
	{
		core::Building building("Count transitions", 12, 3);
		auto const world = buildCountingWorld(building);

		auto const crew = building.addAgentGroup("Crew");
		auto const nightShift = building.addAgentGroup("Night shift");
		auto const alpha = building.createAgent("Alpha", world.frontCorridor, 0, 1.0f);
		auto const beta = building.createAgent("Beta", world.backRoom, 0, 1.0f);

		assign(building, alpha, crew);
		require(building.getAgentGroupMemberCount(crew) == 1
			&& building.getAgentGroupMemberCount(nightShift) == 0,
			"Assigning the first Agent did not move the counts at once: " + allCounts(building));

		// Reassigning is one Agent leaving one group and joining another: both
		// counts move, in the same call, and neither keeps a straggler.
		assign(building, alpha, nightShift);
		require(building.getAgentGroupMemberCount(crew) == 0,
			"Reassigning an Agent left its previous group counting it");
		require(building.getAgentGroupMemberCount(nightShift) == 1,
			"Reassigning an Agent did not reach its new group's count");

		assign(building, beta, nightShift);
		require(building.getAgentGroupMemberCount(nightShift) == 2,
			"A second member did not reach the group's count: " + allCounts(building));

		assign(building, beta, {});
		require(building.getAgentGroupMemberCount(nightShift) == 1,
			"Clearing an Agent left its group counting it");
		require(building.getAgentGroupMemberCount(crew) == 0,
			"Clearing an Agent disturbed a group it was never in: " + allCounts(building));

		// Clearing an Agent that has nothing to clear is a no-op, not a
		// second decrement: a count cannot fall below the zero it sits at.
		assign(building, beta, {});
		require(building.getAgentGroupMemberCount(crew) == 0
			&& building.getAgentGroupMemberCount(nightShift) == 1,
			"Clearing an unassigned Agent changed a count: " + allCounts(building));

		// A refused assignment moves nothing, in either direction.
		std::string diagnostic;
		require(!building.setAgentGroup(alpha, core::AgentGroupId{ 4242 }, &diagnostic),
			"Assigning to an unknown Agent group succeeded");
		require(building.getAgentGroupMemberCount(nightShift) == 1,
			"A refused assignment changed a count");
		require(!building.setAgentGroup(core::AgentId{ 777 }, crew, &diagnostic),
			"Assigning an unknown Agent succeeded");
		require(allCounts(building) == "Crew=0;Night shift=1;",
			"A refused assignment moved the counts: " + allCounts(building));
	}

	// A rename moves a label and nothing else. The count rides on the ID, so
	// the number a group shows is undisturbed by what it is called.
	void aRenameLeavesTheCountAlone()
	{
		core::Building building("Count rename", 12, 3);
		auto const world = buildCountingWorld(building);

		auto const crew = building.addAgentGroup("Crew");
		auto const nightShift = building.addAgentGroup("Night shift");
		assign(building, building.createAgent("One", world.frontCorridor, 0, 1.0f), crew);
		assign(building, building.createAgent("Two", world.frontRoom, 0, 1.0f), crew);
		assign(building, building.createAgent("Three", world.backRoom, 0, 1.0f), crew);
		assign(building, building.createAgent("Four", world.backCorridor, 0, 1.0f), nightShift);

		auto const before = allCounts(building);
		require(before == "Crew=3;Night shift=1;",
			"The rename fixture is not what the check expects: " + before);

		std::string diagnostic;
		require(building.renameAgentGroup(crew, "Facilities team", &diagnostic),
			("Renaming a populated Agent group was refused: " + diagnostic).c_str());

		require(building.getAgentGroupMemberCount(crew) == 3,
			"Renaming an Agent group changed its count");
		require(building.getAgentGroupMemberCount(nightShift) == 1,
			"Renaming one Agent group changed another's count");
		require(agentGroupMemberCountLabel(building, crew) == "3",
			"The Agents column's count did not survive a rename of its group");

		// Renaming back, and renaming the other group too, still moves no
		// number.
		require(building.renameAgentGroup(crew, "Crew", &diagnostic)
			&& building.renameAgentGroup(nightShift, "Graveyard", &diagnostic),
			("A follow-up rename was refused: " + diagnostic).c_str());
		require(building.getAgentGroupMemberCount(crew) == 3
			&& building.getAgentGroupMemberCount(nightShift) == 1,
			"A second rename changed a count: " + allCounts(building));

		// A refused rename changes nothing either - counts included.
		require(!building.renameAgentGroup(crew, "Graveyard", &diagnostic),
			"A rename onto a name another group holds was accepted");
		require(building.getAgentGroupMemberCount(crew) == 3
			&& building.getAgentGroupMemberCount(nightShift) == 1,
			"A refused rename moved a count: " + allCounts(building));
	}

	// Movement is not membership. Agents sent walking, held waiting at a Door
	// they cannot open, and left idle keep whatever count their assignment
	// gave them - whatever Sector they end up in and whatever state they are
	// in while getting there.
	void movementNeverChangesACount()
	{
		core::Building building("Count movement", 12, 3);
		auto const frontCorridor = building.addCorridor(0, 0, 8);
		auto const backRoom = building.addRoom("Back room", 1, 0, 0, 8, 1);
		// A marker to walk to, and a Door that will not open: the two ways an
		// Agent stops being idle in a way that has nothing to do with groups.
		uint32_t destinationIdentifier{ 0x434e5431u };
		building.addSectorMarker(frontCorridor, 0, 7.5f, &destinationIdentifier);
		core::Building::CreateDoorOptions unavailable;
		unavailable.activationMode = core::DoorActivationMode::Unavailable;
		building.addSectorDoor(0, 0, 4, unavailable);
		building.finishBuild();

		auto const crew = building.addAgentGroup("Crew");
		auto const nightShift = building.addAgentGroup("Night shift");

		auto const walker = building.createAgent("Walker", frontCorridor, 0, 0.5f);
		// Started close to the Door so the wait for it to be refused is short
		// rather than a walk across the whole Corridor.
		auto const waiter = building.createAgent("Waiter", frontCorridor, 0, 3.6f);
		auto const idler = building.createAgent("Idler", frontCorridor, 0, 6.5f);
		auto const backAgent = building.createAgent("Back hand", backRoom, 0, 1.0f);

		assign(building, walker, crew);
		assign(building, waiter, crew);
		assign(building, idler, crew);
		assign(building, backAgent, nightShift);

		auto const before = allCounts(building);
		require(before == "Crew=3;Night shift=1;",
			"The movement fixture is not what the check expects: " + before);

		auto* walkerAgent = building.lookupAgent(walker).entity;
		auto* waiterAgent = building.lookupAgent(waiter).entity;
		require(walkerAgent != nullptr && waiterAgent != nullptr,
			"The movement fixture could not find its Agents");

		auto const destination = building.getGraph()->getVertexByIdentifier(destinationIdentifier);
		require(destination != nullptr, "The walking Agent's destination vertex is missing");
		auto route = building.getGraph()->calculatePath(walkerAgent, destination);
		require(route && !route->nodes.empty(), "The walking Agent's route could not be calculated");
		walkerAgent->setPath(std::move(route), true);

		// Send the second Agent at the Door it may not open, which is what
		// holds it in the waiting state rather than merely walking.
		std::shared_ptr<const core::Edge> doorEdge;
		for (auto const& candidate : building.getGraph()->getEdges())
		{
			if (candidate && candidate->getType() == core::EdgeType::Door)
			{
				doorEdge = candidate;
				break;
			}
		}
		require(doorEdge != nullptr, "The test Door produced no traversal edge to be denied");
		auto const first = doorEdge->getVertex(0);
		require(first != nullptr, "The test Door edge has no first vertex");
		auto const source = first->getSector()->getIndex() == frontCorridor
			? first : doorEdge->getVertex(1);
		auto const blocked = doorEdge->getOtherVertex(source);
		waiterAgent->setPath(twoNodePath(source, blocked, doorEdge), false);
		building.advanceTick();
		waiterAgent->startPathing();

		// Run until the blocked Agent is actually held at the Door.
		for (uint32_t tick = 0; tick < 240
			&& building.getSimulationSnapshot().traversalRequests.empty(); ++tick)
		{
			building.advanceTick();
		}
		require(waiterAgent->getState() == core::Agent::State::WaitingForTraversal,
			"The blocked Agent never reached the waiting state, so the check was weak");
		require(walkerAgent->getState() != core::Agent::State::Idle,
			"The walking Agent never started moving, so the check was weak");
		auto* idlerAgent = building.lookupAgent(idler).entity;
		require(idlerAgent != nullptr && idlerAgent->getState() == core::Agent::State::Idle,
			"The idle test Agent was moved by something other than an assignment");

		// Three different movement states, two Layers, four Locations - and
		// the counts are exactly what the assignments made them.
		require(allCounts(building) == before,
			"Agent movement changed a group's count: " + before + " -> "
			+ allCounts(building));

		// Keep running: the counts do not drift as ticks pass and Agents go
		// on changing Sector, state, or both.
		auto const settled = allCounts(building);
		for (uint32_t tick = 0; tick < 90; ++tick) building.advanceTick();
		require(allCounts(building) == settled,
			"Counts drifted while the simulation kept running: " + settled + " -> "
			+ allCounts(building));

		// And each count still matches the Agents carrying the ID, wherever
		// those Agents have got to.
		require(building.getAgentGroupMemberCount(crew)
			== countByScanningAgents(building, crew),
			"After movement, a count disagrees with the Agents carrying its ID");
		require(building.getAgentGroupMemberCount(nightShift)
			== countByScanningAgents(building, nightShift),
			"After movement, a second count disagrees with the Agents carrying its ID");
	}

	// An Agent the Building no longer owns cannot still be counted. Removing
	// one takes it out of its group's number with no further bookkeeping,
	// which is the flip side of there being no counter to update.
	void aRemovedAgentLeavesTheCount()
	{
		core::Building building("Count removal", 12, 3);
		auto const world = buildCountingWorld(building);

		auto const crew = building.addAgentGroup("Crew");
		auto const first = building.createAgent("First", world.frontCorridor, 0, 1.0f);
		auto const second = building.createAgent("Second", world.backRoom, 0, 1.0f);
		assign(building, first, crew);
		assign(building, second, crew);
		require(building.getAgentGroupMemberCount(crew) == 2,
			"The removal fixture did not start with two members: " + allCounts(building));

		require(building.removeAgent(first).removed, "Removing a grouped Agent failed");
		require(building.getAgentGroupMemberCount(crew) == 1,
			"A removed Agent is still counted in its group");

		require(building.removeAgent(second).removed, "Removing the second grouped Agent failed");
		require(building.getAgentGroupMemberCount(crew) == 0,
			"A group still counted members after all of them were removed");
		require(countByScanningAgents(building, crew) == 0,
			"An Agent the Building removed still carries the group's ID");
	}

	// Counting a group this Building never issued is refused rather than
	// answered as zero, which would be indistinguishable from a group that
	// happens to be empty and would hide a borrowed or stale ID.
	void countingAnUnknownGroupIsRefused()
	{
		core::Building building("Count unknown group", 12, 3);
		buildCountingWorld(building);
		auto const crew = building.addAgentGroup("Crew");

		bool threw = false;
		try
		{
			(void)building.getAgentGroupMemberCount(core::AgentGroupId{ 90210 });
		}
		catch (core::BuildingException const& error)
		{
			threw = true;
			require(std::string(error.what()).find("90210") != std::string::npos,
				("The unknown-group count refusal did not name the group: "
					+ std::string(error.what())).c_str());
		}
		require(threw, "Counting an Agent group the Building never issued was accepted");

		// The defined group is untouched by the refused question.
		require(building.getAgentGroupMemberCount(crew) == 0,
			"A refused count question changed a real group's count");
	}

	// The serialized document carries group definitions and per-Agent
	// assignments and nothing else: no count field exists to go stale, and a
	// load rebuilds every count from what the Agents came back carrying.
	void countsReturnFromASaveLoadWithoutStoredCountData()
	{
		core::Building building("Count round trip", 12, 3);
		auto const world = buildCountingWorld(building);

		auto const crew = building.addAgentGroup("Crew");
		auto const nightShift = building.addAgentGroup("Night shift");
		building.addAgentGroup("Unstaffed");
		assign(building, building.createAgent("One", world.frontCorridor, 0, 1.0f), crew);
		assign(building, building.createAgent("Two", world.frontRoom, 0, 1.0f), crew);
		assign(building, building.createAgent("Three", world.backRoom, 0, 1.0f), nightShift);
		building.createAgent("Ungrouped", world.backCorridor, 0, 4.0f);

		auto const before = allCounts(building);
		require(before == "Crew=2;Night shift=1;Unstaffed=0;",
			"The round-trip fixture is not what the check expects: " + before);

		auto const yaml = serializeBuilding(building);
		require(yaml.find("version: 9") != std::string::npos,
			"The count document was written without a version 9 schema");

		// Neither half of the document writes the number down. The groups say
		// what they are called, the Agents say which one they belong to, and
		// the count is derived from the pair.
		requireNoCountShapedKeys(yaml, "agentGroups");
		requireNoCountShapedKeys(yaml, "agents");

		auto const loaded = loadBuilding(yaml);
		require(loaded->getAgentGroupCount() == 3,
			"The reloaded Building does not hold the groups that were saved");
		require(allCounts(*loaded) == before,
			"Counts did not come back from the round trip: " + before + " -> "
			+ allCounts(*loaded));

		// Re-saving produces the same document, so the loaded Building is
		// neither drifting its memberships nor inventing anything to store.
		require(serializeBuilding(*loaded) == yaml,
			"Re-saving a reloaded Building produced a different document");

		// And the reloaded Building keeps counting live: an assignment made
		// after the load moves the count exactly as it did before.
		assign(*loaded, loaded->createAgent("Four", world.frontCorridor, 0, 2.0f), crew);
		require(loaded->getAgentGroupMemberCount(crew) == 3,
			"A count stopped tracking after a load: " + allCounts(*loaded));
	}

	// The undo snapshot is the other restoration path. A snapshot holds the
	// memberships, not the counts; restoring one brings the counts back as a
	// consequence of the memberships, not as a remembered number.
	void countsReturnFromARestoredUndoSnapshot()
	{
		resetUndoHistory();

		auto const building = std::make_shared<core::Building>("Count undo", 12, 3);
		auto const world = buildCountingWorld(*building);

		auto const crew = building->addAgentGroup("Crew");
		auto const nightShift = building->addAgentGroup("Night shift");
		auto const alpha = building->createAgent("Alpha", world.frontCorridor, 0, 1.0f);
		auto const beta = building->createAgent("Beta", world.backRoom, 0, 1.0f);
		assign(*building, alpha, crew);

		require(allCounts(*building) == "Crew=1;Night shift=0;",
			"The undo fixture is not what the check expects: " + allCounts(*building));

		// Snapshot this state, then move both Agents about.
		auto const snapshot = captureDocumentSnapshot(building);
		require(snapshot.has_value(), "The undo snapshot could not be captured");

		assign(*building, beta, crew);
		assign(*building, alpha, nightShift);
		require(building->getAgentGroupMemberCount(crew) == 1
			&& building->getAgentGroupMemberCount(nightShift) == 1,
			"The live counts before the restore are wrong: " + allCounts(*building));

		// The snapshot carries no count data either - only the definitions
		// and the assignments, which is exactly what restoring needs.
		requireNoCountShapedKeys(snapshot->yaml, "agentGroups");
		requireNoCountShapedKeys(snapshot->yaml, "agents");

		// Restoring is a load of the snapshot's document over the live
		// Building, which is how the editor's undo works.
		restoreInto(*building, snapshot->yaml);
		require(building->getAgentGroupMemberCount(crew) == 1,
			"Restoring the snapshot did not rebuild the group's count: "
			+ allCounts(*building));
		require(building->getAgentGroupMemberCount(nightShift) == 0,
			"Restoring the snapshot left the other group counting a member: "
			+ allCounts(*building));
		require(building->getAgentGroup(alpha) == crew,
			"Restoring the snapshot did not restore the assignment the count reads");
		require(!building->getAgentGroup(beta),
			"Restoring the snapshot left an Agent assigned that was unassigned before");

		// And the restored Building counts live from here on.
		assign(*building, beta, nightShift);
		require(building->getAgentGroupMemberCount(nightShift) == 1,
			"A restored Building stopped counting: " + allCounts(*building));
	}

	// The panel's own column list is what the table is built from, and the
	// Agents column is in it, after the name and before the Delete control.
	void theGroupsTableDeclaresAnAgentsColumn()
	{
		auto const& columns = agentGroupsPanelColumns();
		require(columns.size() == 3,
			("The Groups table does not have a name column, an Agents column and a "
				"Delete column; it has "
				+ std::to_string(columns.size())).c_str());
		require(columns[0] == "Name",
			"The Groups table's first column is not the name column");
		require(columns[1] == "Agents",
			"The Groups table's count column is not called Agents");
		require(columns[2] == "Delete",
			"The Groups table's third column is not the Delete column");
	}

	// The real panel, rendered for real, with the count column in it. What
	// matters is that rendering counts leaves no ImGui state behind: a
	// leaked disabled scope once dimmed the rest of the editor for every
	// frame.
	void theCountColumnRendersWithoutLeakingImGuiState()
	{
		ImGuiGuard guard;

		auto const shared = std::make_shared<core::Building>("Count panel", 12, 3);
		auto const world = buildCountingWorld(*shared);
		auto const crew = shared->addAgentGroup("Crew");
		auto const nightShift = shared->addAgentGroup("Night shift");
		shared->addAgentGroup("Unstaffed");
		assign(*shared, shared->createAgent("One", world.frontCorridor, 0, 1.0f), crew);
		assign(*shared, shared->createAgent("Two", world.backRoom, 0, 1.0f), crew);
		assign(*shared, shared->createAgent("Three", world.backCorridor, 0, 1.0f), nightShift);

		for (bool const paused : { true, false })
		{
			if (paused) shared->pauseSimulation();
			else if (shared->isSimulationPaused()) shared->resumeSimulation();

			ImGui::NewFrame();
			ImGui::Begin("Building");

			auto const depthOnEntry = GImGui->DisabledStackSize;
			auto const flagsOnEntry = GImGui->CurrentItemFlags;
			auto const alphaOnEntry = GImGui->Style.Alpha;

			renderAgentGroupsPanel(shared);

			require(GImGui->DisabledStackSize == depthOnEntry,
				"The Agent groups panel left a disabled scope open");
			require(GImGui->CurrentItemFlags == flagsOnEntry,
				"The Agent groups panel changed the current item flags");
			// Compared as bits, not as floats: the question is whether the value
			// is exactly the one stored, which is what "unchanged" means here
			// and what -Wfloat-equal objects to otherwise.
			require(std::bit_cast<uint32_t>(GImGui->Style.Alpha)
				== std::bit_cast<uint32_t>(alphaOnEntry),
				"The Agent groups panel changed the global alpha");
			// Merely drawing the counts changed nothing about them.
			require(allCounts(*shared) == "Crew=2;Night shift=1;Unstaffed=0;",
				"Rendering the counts changed them: " + allCounts(*shared));

			ImGui::End();

			// The frame has to complete, which is where ImGui's own end-frame
			// checks would fire on an unbalanced window.
			ImGui::Render();
		}
	}

	// The count cell on its own, rendered inside a two-column table built the
	// way the Groups table builds its own, and checked against the numbers it
	// has to show as membership changes underneath it.
	void theCountCellShowsTheLiveCount()
	{
		ImGuiGuard guard;

		core::Building building("Count cell", 12, 3);
		auto const world = buildCountingWorld(building);
		auto const crew = building.addAgentGroup("Crew");
		auto const empty = building.addAgentGroup("Unstaffed");

		require(agentGroupMemberCountLabel(building, crew) == "0",
			"A new group's cell does not read zero before its first member");
		require(agentGroupMemberCountLabel(building, empty) == "0",
			"An empty group's cell does not read zero");

		assign(building, building.createAgent("One", world.frontCorridor, 0, 1.0f), crew);
		require(agentGroupMemberCountLabel(building, crew) == "1",
			"A cell does not show one member after one assignment");

		assign(building, building.createAgent("Two", world.backRoom, 0, 1.0f), crew);
		require(agentGroupMemberCountLabel(building, crew) == "2",
			"A cell does not show two members after a second assignment");

		ImGui::NewFrame();
		ImGui::Begin("Building");

		ImGuiTableFlags const flags =
			ImGuiTableFlags_SizingStretchSame |
			ImGuiTableFlags_BordersOuter |
			ImGuiTableFlags_BordersV;
		require(ImGui::BeginTable("AgentGroups", 2, flags),
			"The test Groups table could not be opened");
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Agents");
		ImGui::TableHeadersRow();

		ImGui::TableNextRow();
		ImGui::PushID(crew.value);
		ImGui::TableSetColumnIndex(1);

		auto const depthOnEntry = GImGui->DisabledStackSize;
		auto const flagsOnEntry = GImGui->CurrentItemFlags;

		renderAgentGroupMemberCountCell(building, crew);

		require(GImGui->DisabledStackSize == depthOnEntry,
			"The Agents count cell left a disabled scope open");
		require(GImGui->CurrentItemFlags == flagsOnEntry,
			"The Agents count cell changed the current item flags");
		require(agentGroupMemberCountLabel(building, crew) == "2",
			"Merely rendering the count cell changed the count");

		ImGui::PopID();
		ImGui::EndTable();
		ImGui::End();
		ImGui::Render();
	}
}

void runAgentGroupCountSmokeChecks()
{
	aCountCoversEveryLayerAndSector();
	anEmptyGroupCountsZero();
	countsTrackAssignReassignAndClearImmediately();
	aRenameLeavesTheCountAlone();
	movementNeverChangesACount();
	aRemovedAgentLeavesTheCount();
	countingAnUnknownGroupIsRefused();
	countsReturnFromASaveLoadWithoutStoredCountData();
	countsReturnFromARestoredUndoSnapshot();
	theGroupsTableDeclaresAnAgentsColumn();
	theCountColumnRendersWithoutLeakingImGuiState();
	theCountCellShowsTheLiveCount();
}
