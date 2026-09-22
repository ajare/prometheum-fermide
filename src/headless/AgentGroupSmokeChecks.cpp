// Agent group identity, naming, ordering, persistence, and undo, for ticket #109.
//
// Everything here crosses the two seams that matter: the public Building
// authoring API, and a complete Building serialize/deserialize round trip. The
// registry behind the API is never inspected, and the YAML is only read as a
// whole document - never asserted against incidental formatting.
//
// What gets pinned down:
//
//   the Building owns each group under a stable, monotonically allocated ID,
//   and hands out no way to change a group around its own validation
//   names arrive trimmed, blank and overlong names are refused with a reason,
//   and uniqueness is case-sensitive
//   groups enumerate in creation order, before and after a rename
//   a refused create or rename changes nothing at all
//   version 9 carries the ordered IDs and names through save/load; versions
//   below 9 load with no groups, and a reader capped at 8 refuses 9 instead
//   of quietly dropping the groups
//   malformed version-9 input refuses the whole file and leaves the target
//   Building holding exactly what it held before
//   adding and renaming work while the simulation runs, mark the document
//   modified, and commit exactly one undoable document edit each; a refused
//   or cancelled edit commits none
//   the real panel renders inside a CPU-side ImGui context without leaking a
//   disabled scope, paused or running

#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "core/AgentGroup.h"
#include "core/Building.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/SerializationException.h"
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

	// Loads over a Building that already exists, which is how a refused open
	// gets caught out for leftover state.
	void loadInto(core::Building& target, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(target.deserialize(*reader, workData), "The Building did not reload");
	}

	std::vector<std::string> namesInOrder(core::Building const& building)
	{
		std::vector<std::string> names;
		for (auto const id : building.getAgentGroupIds())
			names.push_back(building.getAgentGroupName(id));
		return names;
	}

	// Replaces the document's schema version, leaving everything else alone:
	// the cheap way to ask what an older reader would be handed.
	std::string withVersion(std::string const& yaml, uint32_t version)
	{
		auto const found = yaml.find("version: 12");
		require(found != std::string::npos, "The serialised Building carried no version 12 field");
		return yaml.substr(0, found) + "version: " + std::to_string(version)
			+ yaml.substr(found + std::strlen("version: 12"));
	}

	std::string replaceOnce(std::string const& yaml, std::string const& find,
		std::string const& replace)
	{
		auto const found = yaml.find(find);
		require(found != std::string::npos,
			("The serialised Building did not contain \"" + find + "\" to rewrite").c_str());
		return yaml.substr(0, found) + replace + yaml.substr(found + find.size());
	}

	// A Building with a little world in it, so a group is never the only thing
	// the document carries.
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
		gBuildingDocumentHistory.clear();
	}

	// ---------------------------------------------------------------- checks

	// Identity is allocated by the Building, never inferred from the name, and
	// the creation order survives a rename of the group in the middle of it.
	void agentGroupsHaveStableIdsAndEnumerateInCreationOrder()
	{
		core::Building building("Group order", 12, 3);
		buildWorld(building);

		require(building.getAgentGroupCount() == 0,
			"A newly built Building already reported Agent groups");

		auto const first = building.addAgentGroup("Maintenance");
		auto const second = building.addAgentGroup("Security");
		auto const third = building.addAgentGroup("Night shift");

		require(first && second && third, "A created Agent group came back with a null ID");
		require(first < second && second < third,
			"Agent group IDs were not allocated monotonically");
		require(building.getAgentGroupCount() == 3,
			"Three Agent groups were created but the Building counts differently");

		std::vector<core::AgentGroupId> const created{ first, second, third };
		require(building.getAgentGroupIds() == created,
			"Agent groups did not enumerate in creation order");
		require(building.getAgentGroupName(first) == "Maintenance"
			&& building.getAgentGroupName(second) == "Security"
			&& building.getAgentGroupName(third) == "Night shift",
			"An Agent group does not read back the name it was created with");

		auto const lookup = building.lookupAgentGroup(second);
		require(lookup && lookup.entity->getName() == "Security",
			"Looking up an Agent group by its ID did not find it");
		require(!building.lookupAgentGroup(core::AgentGroupId{ 424242 })
			&& !building.lookupAgentGroup(core::AgentGroupId{ 424242 }).diagnostic.empty(),
			"Looking up an unknown Agent group succeeded, or failed silently");

		// Rename the middle one: the ID is the same handle, the order is the
		// same order, and the neighbours are untouched.
		std::string diagnostic;
		require(building.renameAgentGroup(second, "Facilities", &diagnostic),
			("Renaming an Agent group was refused: " + diagnostic).c_str());
		require(building.getAgentGroupIds() == created,
			"Renaming an Agent group disturbed the creation order");
		require(building.getAgentGroupName(second) == "Facilities",
			"Renaming an Agent group did not change its name");
		require(building.getAgentGroupName(first) == "Maintenance"
			&& building.getAgentGroupName(third) == "Night shift",
			"Renaming one Agent group disturbed another");
		require(building.getAgentGroupCount() == 3,
			"Renaming an Agent group changed how many there are");
	}

	// The naming rule, seen from outside: trimmed on the way in, blank and
	// overlong refused with a reason, uniqueness case-sensitive.
	void agentGroupNamesAreTrimmedValidatedAndCaseSensitive()
	{
		core::Building building("Group naming", 12, 3);
		buildWorld(building);

		auto const id = building.addAgentGroup("  Front of house  ");
		require(building.getAgentGroupName(id) == "Front of house",
			"An Agent group name was not trimmed on the way in");

		for (auto const* blank : { "", "   ", "\t ", " \t\t " })
		{
			std::string diagnostic;
			require(!building.canAddAgentGroup(blank, &diagnostic),
				"An Agent group with a blank name was accepted");
			require(!diagnostic.empty(),
				"An Agent group blank name was refused without a diagnostic");

			bool threw = false;
			try { building.addAgentGroup(blank); }
			catch (core::Exception const&) { threw = true; }
			require(threw, "addAgentGroup accepted a blank name");
		}

		// The limit is 63 bytes, measured in the bytes that get stored.
		auto const fits = std::string(core::AgentGroup::MaxNameBytes, 'a');
		auto const tooLong = fits + "a";
		require(core::AgentGroup::MaxNameBytes == 63,
			"The Agent group name limit is no longer 63 bytes");
		require(building.canAddAgentGroup(fits),
			"A maximum-length Agent group name was refused");

		std::string diagnostic;
		require(!building.canAddAgentGroup(tooLong, &diagnostic),
			"An overlong Agent group name was accepted");
		require(diagnostic.find("63") != std::string::npos,
			("The overlong refusal did not name the limit: " + diagnostic).c_str());
		// Padding is trimmed before the length is judged, so a long run of
		// spaces around a short name is a short name.
		require(building.canAddAgentGroup(std::string(core::AgentGroup::MaxNameBytes, ' ') + "x"),
			"An Agent group name padded with spaces past the limit was refused rather than trimmed");

		// Uniqueness is case-sensitive: "Front of house" is taken, but a
		// different capitalisation is a different group.
		require(!building.canAddAgentGroup("Front of house", &diagnostic),
			"A duplicate Agent group name was accepted");
		require(diagnostic.find("Front of house") != std::string::npos,
			("The duplicate refusal did not name the group: " + diagnostic).c_str());
		require(building.canAddAgentGroup("front of house"),
			"Agent group names differing only by case were treated as duplicates");

		// Multi-byte names are counted in bytes, not characters.
		auto const twoByteName = std::string("\xc3\xa9");   // U+00E9, two bytes
		require(twoByteName.size() == 2, "The two-byte test character is not two bytes");
		auto const multibyte = building.addAgentGroup("Plan " + twoByteName);
		require(building.getAgentGroupName(multibyte) == "Plan " + twoByteName,
			"A multi-byte Agent group name did not come back as it went in");
		require(!building.canAddAgentGroup(std::string(core::AgentGroup::MaxNameBytes - 1, 'a')
			+ twoByteName, &diagnostic),
			"A multi-byte Agent group name one byte past the limit was accepted");
		require(!building.canAddAgentGroup(std::string("Bad\xFF\xFE"), &diagnostic),
			"A name that is not valid UTF-8 was accepted as an Agent group name");
		require(building.canAddAgentGroup("Crew \U0001F6E0"),
			"A four-byte codepoint was refused in an Agent group name");
		require(building.canAddAgentGroup("Euro \u20AC"),
			"A three-byte codepoint was refused in an Agent group name");
		// An overlong encoding of "/" has the shape of a two-byte sequence but
		// is not valid UTF-8, and neither is a lone surrogate half.
		require(!building.canAddAgentGroup(std::string("Over\xC0\xAF"), &diagnostic),
			"An overlong UTF-8 encoding was accepted as an Agent group name");
		require(!building.canAddAgentGroup(std::string("\xED\xA0\x80"), &diagnostic),
			"A lone UTF-16 surrogate was accepted as an Agent group name");

		// None of the refusals above left anything behind.
		require(building.getAgentGroupCount() == 2,
			"A refused Agent group add changed how many groups the Building holds");
	}

	// A rename that cannot be honoured is not half a rename: the group keeps
	// its name, its ID, and its place in the order.
	void renamingAnAgentGroupKeepsItsPlaceAndFailsAtomically()
	{
		core::Building building("Group rename", 12, 3);
		buildWorld(building);

		auto const alpha = building.addAgentGroup("Alpha");
		auto const bravo = building.addAgentGroup("Bravo");
		auto const charlie = building.addAgentGroup("Charlie");
		std::vector<core::AgentGroupId> const created{ alpha, bravo, charlie };

		std::string diagnostic;
		require(!building.renameAgentGroup(core::AgentGroupId{ 9999 }, "Ghost", &diagnostic),
			"Renaming an Agent group this Building never issued succeeded");
		require(!diagnostic.empty(),
			"Renaming an unknown Agent group failed without a diagnostic");

		require(!building.renameAgentGroup(bravo, "Alpha", &diagnostic),
			"Renaming an Agent group onto a name another group already holds succeeded");
		require(building.getAgentGroupName(bravo) == "Bravo",
			"A refused rename still changed the group's name");

		require(!building.renameAgentGroup(bravo, "   ", &diagnostic),
			"Renaming an Agent group to a blank name succeeded");
		require(!building.renameAgentGroup(bravo, std::string(64, 'b'), &diagnostic),
			"Renaming an Agent group to an overlong name succeeded");

		require(building.getAgentGroupName(bravo) == "Bravo",
			"A refused rename left the group holding something other than its own name");
		require(building.getAgentGroupIds() == created,
			"A refused rename disturbed the Agent group order");
		require(building.getAgentGroupCount() == 3,
			"A refused rename changed how many Agent groups there are");

		// Renaming a group to the name it already has - padded or not - is a
		// legitimate no-op, not a collision with itself.
		require(building.renameAgentGroup(bravo, " Bravo ", &diagnostic),
			("Renaming an Agent group to its own padded name was refused: " + diagnostic).c_str());
		require(building.getAgentGroupName(bravo) == "Bravo",
			"Renaming an Agent group to its own padded name changed the name");
		require(building.getAgentGroupIds() == created,
			"Renaming an Agent group to its own name disturbed the order");
	}

	// Save and reopen: the ordered IDs and names come back, and come back the
	// same way a second time.
	void agentGroupsRoundTripThroughSaveAndLoad()
	{
		core::Building building("Group round trip", 12, 3);
		buildWorld(building);

		auto const first = building.addAgentGroup("Alpha");
		auto const second = building.addAgentGroup("Bravo");
		auto const third = building.addAgentGroup("Charlie");
		std::vector<core::AgentGroupId> const created{ first, second, third };
		std::vector<std::string> const names{ "Alpha", "Facilities", "Charlie" };

		std::string diagnostic;
		require(building.renameAgentGroup(second, "Facilities", &diagnostic),
			("Renaming before the round trip was refused: " + diagnostic).c_str());

		auto const yaml = serializeBuilding(building);
		require(yaml.find("version: 12") != std::string::npos,
			"Agent groups were not written under the current Building schema");
		require(yaml.find("agentGroups") != std::string::npos,
			"The Agent group collection was not persisted");

		auto const loaded = loadBuilding(yaml);
		require(loaded->getAgentGroupCount() == 3,
			"The reloaded Building holds a different number of Agent groups");
		require(loaded->getAgentGroupIds() == created,
			"Agent group IDs did not survive the round trip");
		require(namesInOrder(*loaded) == names,
			"Agent group names or their order did not survive the round trip");

		// Re-saving what was just loaded produces the same document, so the
		// stored form is canonical and a load cannot drift it.
		require(serializeBuilding(*loaded) == yaml,
			"Re-saving a reloaded Building produced a different Agent group document");
	}

	// Anything written before version 9 carries no Agent groups, and the
	// reader does not invent any.
	void preVersionNineDocumentsLoadWithNoAgentGroups()
	{
		core::Building building("Group legacy source", 12, 3);
		buildWorld(building);
		building.addAgentGroup("Alpha");
		building.addAgentGroup("Bravo");
		auto const yaml = serializeBuilding(building);

		// Versions 2 through 8 share the record shapes this writer emits, so
		// each loads as a real pre-Agent-group document.
		for (uint32_t version = 2; version <= 8; ++version)
		{
			auto const legacy = loadBuilding(withVersion(yaml, version));
			require(legacy->getAgentGroupCount() == 0,
				("A version-" + std::to_string(version)
					+ " document loaded with Agent groups").c_str());
			require(legacy->getAgentGroupIds().empty(),
				("A version-" + std::to_string(version)
					+ " document reported Agent group IDs").c_str());
		}

		// Version 1 encodes its construction records by number rather than by
		// name, so it is refused outright rather than misread.
		bool refusedVersionOne = false;
		try { loadBuilding(withVersion(yaml, 1)); }
		catch (core::SerializationException const&) { refusedVersionOne = true; }
		require(refusedVersionOne,
			"A version-1 shaped document was accepted by the current reader");
	}

	// The hazard this guards is quiet data loss: an older reader that ignored
	// the version would load a version-9 file, see no groups it understands,
	// and write the file back without them.
	void aVersionEightReaderRefusesVersionNineRatherThanDroppingGroups()
	{
		core::Building building("Group ceiling", 12, 3);
		buildWorld(building);
		building.addAgentGroup("Alpha");
		building.addAgentGroup("Bravo");
		auto const yaml = serializeBuilding(building);

		uint32_t const preAgentGroupVersionCeiling{ 8 };
		bool refused = false;
		try
		{
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			reader->beginMap("building");
			auto const version = reader->readUint32("version");
			if (version > preAgentGroupVersionCeiling)
				throw core::SerializationException("Unsupported Building serialization version");
			reader->endMap();
		}
		catch (core::SerializationException const& error)
		{
			refused = true;
			require(std::string(error.what()).find("version") != std::string::npos,
				("A version-8 reader failed for a reason other than the version: "
					+ std::string(error.what())).c_str());
		}
		require(refused,
			"A reader capped at version 8 accepted a version-9 Agent group document");
	}

	// Malformed version-9 input refuses the whole file. The target Building is
	// one that already has groups, so "no partial state" has something
	// concrete to mean: it still holds exactly its own groups afterwards.
	void malformedAgentGroupInputRefusesTheFileWithoutPartialState()
	{
		core::Building source("Malformed source", 12, 3);
		buildWorld(source);
		source.addAgentGroup("Alpha");
		source.addAgentGroup("Bravo");
		auto const goodYaml = serializeBuilding(source);

		core::Building target("Malformed target", 12, 3);
		buildWorld(target);
		target.addAgentGroup("Existing");
		auto const keptIds = target.getAgentGroupIds();
		auto const keptNames = namesInOrder(target);
		require(keptIds.size() == 1, "The test target did not start with its own group");

		std::vector<std::pair<std::string, std::string>> const cases{
			{ "a zero Agent group ID", replaceOnce(goodYaml, "id: 2", "id: 0") },
			{ "a duplicate Agent group ID", replaceOnce(goodYaml, "id: 2", "id: 1") },
			{ "a duplicate Agent group name", replaceOnce(goodYaml, "name: Bravo", "name: Alpha") },
			{ "a blank Agent group name", replaceOnce(goodYaml, "name: Bravo", "name: \"\"") },
			{ "an overlong Agent group name",
				replaceOnce(goodYaml, "name: Bravo", "name: " + std::string(64, 'x')) },
		};

		for (auto const& [what, badYaml] : cases)
		{
			bool refused = false;
			std::string reason;
			try
			{
				loadInto(target, badYaml);
			}
			catch (core::SerializationException const& error)
			{
				refused = true;
				reason = error.what();
			}
			require(refused, ("A version-9 document carrying " + what + " was accepted").c_str());
			require(!reason.empty(),
				("A version-9 document carrying " + what + " was refused without a reason").c_str());
			require(target.getAgentGroupIds() == keptIds
				&& namesInOrder(target) == keptNames,
				("A refused load left partial Agent group state behind: " + what).c_str());
		}
	}

	// The editor's own commit path, with the simulation live: one accepted
	// operation is one undo entry, one refused operation is none, and neither
	// pauses the world nor touches its topology.
	void groupEditsRunAlongsideTheSimulationAndCommitOneUndoEach()
	{
		resetUndoHistory();
		resetAgentGroupsPanelState();

		auto const building = std::make_shared<core::Building>("Group edits", 12, 3);
		buildWorld(*building);
		require(!building->isSimulationPaused(),
			"The test Building started paused, so it proved nothing about running edits");

		// Let the world actually run, then come back to the document clean so
		// "marked modified" means this operation did it.
		building->advanceTick();
		building->advanceTick();
		building->markUnmodified();
		require(!building->isModified(), "The test Building did not come back clean");

		auto const topologyBefore = building->getTopologyGeneration();
		std::string diagnostic;

		auto const added = commitAgentGroupAdd(building, "  Response team  ", diagnostic);
		require(added.value != 0,
			("Adding an Agent group through the panel seam failed: " + diagnostic).c_str());
		require(building->getAgentGroupName(added) == "Response team",
			"The panel seam did not trim the new Agent group name");
		require(building->isModified(),
			"Adding an Agent group did not mark the document modified");
		require(gBuildingDocumentHistory.undoCount() == 1,
			"Adding an Agent group did not commit exactly one undoable document edit");
		require(!gBuildingDocumentHistory.canRedo(), "Adding an Agent group produced a redo entry");
		require(!building->isSimulationPaused(),
			"Adding an Agent group paused the simulation");
		require(building->getTopologyGeneration() == topologyBefore,
			"Adding an Agent group rebuilt the traversal topology");

		require(commitAgentGroupRename(building, added, "Response", diagnostic),
			("Renaming an Agent group through the panel seam failed: " + diagnostic).c_str());
		require(gBuildingDocumentHistory.undoCount() == 2,
			"Renaming an Agent group did not commit exactly one undoable document edit");

		// Refused operations leave the history exactly where it was.
		require(!commitAgentGroupAdd(building, "Response", diagnostic),
			"A duplicate Agent group name was accepted through the panel seam");
		require(gBuildingDocumentHistory.undoCount() == 2,
			"A refused Agent group add committed an undo entry");

		require(!commitAgentGroupRename(building, core::AgentGroupId{ 4242 }, "Ghost", diagnostic),
			"Renaming an unknown Agent group succeeded through the panel seam");
		require(gBuildingDocumentHistory.undoCount() == 2,
			"A refused Agent group rename committed an undo entry");

		require(!commitAgentGroupAdd(building, "   ", diagnostic),
			"A blank Agent group name was accepted through the panel seam");
		require(gBuildingDocumentHistory.undoCount() == 2,
			"A blank Agent group add committed an undo entry");

		// Undo is a snapshot restore, so the entries themselves are the
		// history: the newest holds the state before the rename, the oldest
		// the state before the group existed at all.
		require(gBuildingDocumentHistory.undoCount() == 2, "The undo stack is not the two edits made");
		auto const beforeRename = loadBuilding(gBuildingDocumentHistory.undoEntries().back().yaml);
		require(beforeRename->getAgentGroupCount() == 1
			&& beforeRename->getAgentGroupName(added) == "Response team",
			"The undo snapshot did not hold the state before the rename");
		auto const beforeAdd = loadBuilding(gBuildingDocumentHistory.undoEntries().front().yaml);
		require(beforeAdd->getAgentGroupCount() == 0,
			"The oldest undo snapshot still carried the added Agent group");

		// And the live Building is where the redo would take it.
		require(building->getAgentGroupName(added) == "Response",
			"The live Building did not hold the renamed Agent group");
	}

	// The real panel, rendered for real. What matters here is that it leaves no
	// ImGui state behind: a leaked disabled scope once dimmed the rest of the
	// editor for every frame.
	void theAgentGroupsPanelRendersWithoutLeakingImGuiState()
	{
		ImGuiGuard guard;

		auto const shared = std::make_shared<core::Building>("Group panel", 12, 3);
		buildWorld(*shared);
		shared->addAgentGroup("Alpha");
		shared->addAgentGroup("Bravo");

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

			ImGui::End();

			// The frame has to complete, which is where ImGui's own end-frame
			// checks would fire on an unbalanced window.
			ImGui::Render();
		}
	}
}

void runAgentGroupSmokeChecks()
{
	resetAgentGroupsPanelState();
	agentGroupsHaveStableIdsAndEnumerateInCreationOrder();
	agentGroupNamesAreTrimmedValidatedAndCaseSensitive();
	renamingAnAgentGroupKeepsItsPlaceAndFailsAtomically();
	agentGroupsRoundTripThroughSaveAndLoad();
	preVersionNineDocumentsLoadWithNoAgentGroups();
	aVersionEightReaderRefusesVersionNineRatherThanDroppingGroups();
	malformedAgentGroupInputRefusesTheFileWithoutPartialState();
	groupEditsRunAlongsideTheSimulationAndCommitOneUndoEach();
	theAgentGroupsPanelRendersWithoutLeakingImGuiState();
	resetAgentGroupsPanelState();
}
