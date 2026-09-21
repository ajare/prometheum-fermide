// Agent group ID allocation across a reload, for ticket #123.
//
// An AgentGroupId is promised to be stable, monotonically allocated and never
// reused, with zero held back as the null handle (EntityId.h, ADR 0006). The
// version-9 document persisted the live {id, name} pairs and nothing else, so
// the allocator's high-water mark died with the Building that held it: delete
// the highest group, save, reopen, and the next group inherited the deleted
// identity. Push the other way - a document whose highest ID is the top of the
// range - and the allocator wrapped onto zero, which is falsey to
// canSetAgentGroup and rejected outright as a group ID on the next open.
//
// What gets pinned down:
//
//   a fresh Building allocates from 1, and every ID it hands out is live and
//   comes after the last
//   the save carries the allocator's mark, so a reopened Building continues
//   from where the writer got to rather than from its survivors
//   delete -> save/reopen -> add never reissues the deleted ID
//   delete -> undo/redo -> add never reissues it either, because a snapshot
//   is a document and carries the same mark
//   undoing a delete restores the group without dragging the allocator back
//   down to its identity
//   a document with every group deleted still keeps the allocator ahead
//   sparse IDs keep the allocator above the highest live ID
//   a document at the top of the range loads as exhausted: it never hands out
//   the null handle, and it saves and reopens as exhausted
//   an explicit zero mark reads as exhaustion, never as a group with ID zero
//   a mark that runs backwards against the groups in the same file is refused
//   with a diagnostic naming both numbers, and the refusal leaves the live
//   Building, its groups and its own allocator exactly as they were
//   documents written before the mark existed still load, and get a safe next
//   ID derived from the groups they do carry
//   through the panel seam an exhausted Building refuses the add, says why,
//   and commits nothing to the undo history

#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Agent.h"
#include "core/Building.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"

#include "AgentGroupsPanel.h"
#include "DocumentEdit.h"

namespace
{
	// 2^64 - 1, spelled out so the checks read as the boundary rather than as
	// an arithmetic expression that might itself be wrong.
	constexpr uint64_t kTopOfRange{ 18446744073709551615ULL };

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

	// A whole-document load into a caller-chosen Building, reporting refusal
	// rather than throwing through the check that asked for it.
	bool tryLoadInto(core::Building& target, std::string const& yaml, std::string& diagnostic)
	{
		core::SerializationWorkData workData;
		try
		{
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			if (!target.deserialize(*reader, workData))
			{
				diagnostic = "The Building reported that it did not reload";
				return false;
			}
		}
		catch (std::exception const& error)
		{
			diagnostic = error.what();
			return false;
		}
		return true;
	}

	std::shared_ptr<core::Building> tryLoad(std::string const& yaml, std::string& diagnostic)
	{
		auto loaded = std::make_shared<core::Building>("Loaded Building", 1, 1);
		if (!tryLoadInto(*loaded, yaml, diagnostic)) return nullptr;
		return loaded;
	}

	std::shared_ptr<core::Building> loadBuilding(std::string const& yaml)
	{
		std::string diagnostic;
		auto loaded = tryLoad(yaml, diagnostic);
		require(loaded != nullptr, "A loadable Agent group document was refused: " + diagnostic);
		return loaded;
	}

	// Change one exact piece of a real save. Every boundary case below is
	// authored by editing a document the writer actually produced rather than
	// by hand-writing YAML, so what the loader meets is one number away from
	// something that really came off the other end of this file.
	std::string replacedOnce(std::string const& yaml, std::string const& from,
		std::string const& to)
	{
		auto const pos = yaml.find(from);
		require(pos != std::string::npos, "The test document does not contain \"" + from + "\"");
		return yaml.substr(0, pos) + to + yaml.substr(pos + from.size());
	}

	std::string withoutLine(std::string const& yaml, std::string const& line)
	{
		return replacedOnce(yaml, line, "");
	}

	// Drop a whole top-level block - its key line and everything indented under
	// it - to hand the loader a document shaped like an older schema.
	std::string withoutTopLevelBlock(std::string const& yaml, std::string const& key)
	{
		std::string out;
		std::istringstream in(yaml);
		std::string line;
		std::string const header = key + ":";
		bool skipping{ false };

		while (std::getline(in, line))
		{
			if (!line.empty() && line[0] != ' ' && line[0] != '\t')
			{
				skipping = line.compare(0, header.size(), header) == 0;
			}
			if (skipping) continue;
			out += line + "\n";
		}
		return out;
	}

	// The `nextAgentGroupId:` value a document carries, if it carries one.
	bool persistedAllocatorMark(std::string const& yaml, uint64_t& value)
	{
		std::istringstream in(yaml);
		std::string line;
		std::string const key{ "nextAgentGroupId:" };

		while (std::getline(in, line))
		{
			auto const start = line.find_first_not_of(" \t");
			if (start == std::string::npos) continue;
			if (line.compare(start, key.size(), key) != 0) continue;

			auto const rest = line.substr(start + key.size());
			auto const valueStart = rest.find_first_not_of(" \t");
			require(valueStart != std::string::npos,
				"The document's nextAgentGroupId field carries no value");
			value = std::stoull(rest.substr(valueStart));
			return true;
		}
		return false;
	}

	std::string allocatorMarkText(std::string const& yaml)
	{
		uint64_t value{ 0 };
		return persistedAllocatorMark(yaml, value) ? std::to_string(value) : std::string("<absent>");
	}

	// Every group the Building reports, ID and name, as one comparable line.
	std::string groupSummary(core::Building const& building)
	{
		std::string out;
		for (auto const id : building.getAgentGroupIds())
		{
			out += std::to_string(id.value) + ':' + building.getAgentGroupName(id) + ';';
		}
		return out;
	}

	// No group anywhere may carry the null handle: it reads as "no Agent group"
	// to an assignment and is refused as a group ID by the next save's reader.
	void requireNoNullAgentGroupId(core::Building const& building, std::string const& context)
	{
		for (auto const id : building.getAgentGroupIds())
		{
			require(id.value != 0,
				"An Agent group holds the null AgentGroupId (" + context + ")");
		}
	}

	// Two Locations on the front Layer and one behind, so a group's Building is
	// a Building rather than a single cell.
	void buildWorld(core::Building& building)
	{
		building.addCorridor(0, 0, 12);
		building.addRoom("Front room", 0, 2, 0, 6, 1);
		building.finishBuild();
	}

	void resetUndoHistory()
	{
		gBuildingDocumentHistory.clear();
	}

	// The editor's own undo and redo, the same shape as UI.cpp's
	// restoreDocumentSnapshot(): the live state crosses to the other stack and
	// the newest snapshot on the source stack becomes the live Building.
	void stepDocument(std::shared_ptr<core::Building>& building, bool redo)
	{
		auto const current = captureDocumentSnapshot(building);
		require(current.has_value(), "The live document could not be captured");

		std::shared_ptr<core::Building> loaded;
		auto restore = [&loaded](DocumentSnapshot const& target)
		{
			loaded = loadBuilding(target.yaml);
			loaded->markModified();
			return true;
		};
		auto const restored = redo
			? gBuildingDocumentHistory.redo(current, restore)
			: gBuildingDocumentHistory.undo(current, restore);
		require(restored, redo ? "There is no redo entry to restore"
			: "There is no undo entry to restore");
		building = std::move(loaded);
		resetAgentGroupsPanelState();
	}

	void undoDocument(std::shared_ptr<core::Building>& building) { stepDocument(building, false); }
	void redoDocument(std::shared_ptr<core::Building>& building) { stepDocument(building, true); }

	// A document whose allocator has nothing left to give: two live groups and
	// a mark of zero, which is what an exhausted save writes.
	std::string exhaustedDocument()
	{
		core::Building building("Exhausted source", 12, 3);
		buildWorld(building);
		building.addAgentGroup("Kept");
		building.addAgentGroup("Also kept");
		return replacedOnce(serializeBuilding(building),
			"nextAgentGroupId: 3\n", "nextAgentGroupId: 0\n");
	}

	// A document whose mark sits behind a group the same document defines:
	// allocating from it would collide with a live identity. The mark and the
	// highest defined ID come back with it, so a check can insist the refusal
	// names the two numbers that actually disagree.
	struct MalformedMarkDocument
	{
		std::string yaml;
		uint64_t mark{ 0 };
		uint64_t highestDefined{ 0 };
	};

	MalformedMarkDocument backwardsMarkDocument()
	{
		core::Building building("Backwards source", 12, 3);
		buildWorld(building);
		building.addAgentGroup("One");
		building.addAgentGroup("Two");
		auto const three = building.addAgentGroup("Three");
		std::string diagnostic;
		require(building.deleteAgentGroup(three, &diagnostic),
			"Deleting the highest Agent group failed: " + diagnostic);

		// The survivors are 1 and 2 with a mark of 4; move the top survivor to
		// 7 and the mark is left behind it.
		MalformedMarkDocument malformed;
		malformed.yaml = replacedOnce(serializeBuilding(building), "  - id: 2\n", "  - id: 7\n");
		malformed.highestDefined = 7;
		require(persistedAllocatorMark(malformed.yaml, malformed.mark)
			&& malformed.mark == 4,
			"The malformed fixture did not leave the mark where it should be: "
				+ allocatorMarkText(malformed.yaml));
		require(malformed.mark < malformed.highestDefined,
			"The malformed fixture is not malformed: the mark is still ahead of its groups");
		return malformed;
	}

	// ---------------------------------------------------------------- checks

	// The baseline the rest of the file leans on.
	void aFreshBuildingIssuesLiveIdsInOrder()
	{
		core::Building building("Fresh allocation", 12, 3);
		buildWorld(building);

		uint64_t previous{ 0 };
		for (int index = 1; index <= 4; ++index)
		{
			auto const id = building.addAgentGroup("Group " + std::to_string(index));
			require(static_cast<bool>(id), "A fresh Building issued the null AgentGroupId");
			require(id.value > previous,
				"A fresh AgentGroupId did not come after the one issued before it");
			previous = id.value;
		}

		require(groupSummary(building) == "1:Group 1;2:Group 2;3:Group 3;4:Group 4;",
			"A fresh Building did not allocate from 1: " + groupSummary(building));
		requireNoNullAgentGroupId(building, "after a fresh allocation run");
	}

	// The reported bug. Delete the highest group, save, reopen, add: the
	// replacement used to inherit the deleted group's identity.
	void theHighestDeletedAgentGroupIdIsNotReissuedAcrossASaveAndReopen()
	{
		core::Building building("Save and reopen", 12, 3);
		buildWorld(building);

		auto const kept = building.addAgentGroup("Kept");
		auto const deleted = building.addAgentGroup("Deleted");
		require(kept.value == 1 && deleted.value == 2,
			"The fixture did not allocate 1 and 2: " + groupSummary(building));

		std::string diagnostic;
		require(building.deleteAgentGroup(deleted, &diagnostic),
			"Deleting the highest Agent group failed: " + diagnostic);

		auto const reopened = loadBuilding(serializeBuilding(building));
		require(groupSummary(*reopened) == "1:Kept;",
			"The reopened Building did not come back with just the kept group: "
				+ groupSummary(*reopened));

		auto const replacement = reopened->addAgentGroup("Replacement");
		require(static_cast<bool>(replacement),
			"The first Agent group after a reopen was the null handle");
		require(replacement != deleted,
			"The reopened Building reissued the deleted Agent group's identity");
		require(replacement.value > deleted.value,
			"The replacement AgentGroupId did not come after the deleted one: "
				+ std::to_string(replacement.value) + " is not past "
				+ std::to_string(deleted.value));
		require(replacement.value == 3,
			"The reopened Building did not continue from the mark the save left behind: "
				+ std::to_string(replacement.value));
		requireNoNullAgentGroupId(*reopened, "after a save, reopen and add");
	}

	// The mark has to be in the file, not merely working inside one process: a
	// save that cannot say where its allocator got to is the whole bug.
	void theSaveCarriesTheAllocatorMark()
	{
		core::Building building("Persisted mark", 12, 3);
		buildWorld(building);
		building.addAgentGroup("Kept");
		auto const deleted = building.addAgentGroup("Deleted");
		std::string diagnostic;
		require(building.deleteAgentGroup(deleted, &diagnostic),
			"Deleting the highest Agent group failed: " + diagnostic);

		auto const yaml = serializeBuilding(building);
		uint64_t mark{ 0 };
		require(persistedAllocatorMark(yaml, mark),
			"The saved document carries no nextAgentGroupId field");
		require(mark == 3,
			"The saved allocator mark is not one past the highest ID ever issued: "
				+ std::to_string(mark));
	}

	// Undo and redo travel as serialized documents, so the mark that fixes
	// save/reopen has to fix them too. Delete, undo, redo, add: the added
	// group must not land on the deleted identity.
	void theHighestDeletedAgentGroupIdIsNotReissuedAcrossUndoAndRedo()
	{
		resetUndoHistory();
		auto building = std::make_shared<core::Building>("Undo and redo", 12, 3);
		buildWorld(*building);

		std::string diagnostic;
		auto const kept = commitAgentGroupAdd(building, "Kept", diagnostic);
		require(static_cast<bool>(kept), "The panel refused the first Agent group: " + diagnostic);
		auto const deleted = commitAgentGroupAdd(building, "Deleted", diagnostic);
		require(static_cast<bool>(deleted), "The panel refused the second Agent group: " + diagnostic);
		require(commitAgentGroupDelete(building, deleted, diagnostic),
			"The panel refused the Agent group delete: " + diagnostic);

		undoDocument(building);
		require(building->getAgentGroupCount() == 2,
			"Undo did not bring the deleted Agent group back: " + groupSummary(*building));
		redoDocument(building);
		require(groupSummary(*building) == "1:Kept;",
			"Redo did not take the Agent group away again: " + groupSummary(*building));

		auto const replacement = commitAgentGroupAdd(building, "Replacement", diagnostic);
		require(static_cast<bool>(replacement),
			"The panel refused an Agent group added after undo and redo: " + diagnostic);
		require(replacement != deleted && replacement.value > deleted.value,
			"An Agent group added after undo and redo inherited the deleted identity: "
				+ std::to_string(replacement.value));
		require(replacement.value == 3,
			"The Agent group added after undo and redo did not continue from the mark: "
				+ std::to_string(replacement.value));
		requireNoNullAgentGroupId(*building, "after undo and redo");
		resetUndoHistory();
	}

	// The other half of the same round trip: undo puts the group back with its
	// own identity, and the allocator still has to be standing above it.
	void undoingADeleteKeepsTheAllocatorAboveTheRestoredIdentity()
	{
		resetUndoHistory();
		auto building = std::make_shared<core::Building>("Undo restore", 12, 3);
		buildWorld(*building);

		std::string diagnostic;
		require(static_cast<bool>(commitAgentGroupAdd(building, "Kept", diagnostic)),
			"The panel refused the first Agent group: " + diagnostic);
		auto const deleted = commitAgentGroupAdd(building, "Deleted", diagnostic);
		require(static_cast<bool>(deleted), "The panel refused the second Agent group: " + diagnostic);
		require(commitAgentGroupDelete(building, deleted, diagnostic),
			"The panel refused the Agent group delete: " + diagnostic);

		undoDocument(building);
		require(static_cast<bool>(building->lookupAgentGroup(deleted)),
			"Undo did not restore the deleted Agent group");

		auto const next = commitAgentGroupAdd(building, "Next", diagnostic);
		require(static_cast<bool>(next),
			"The panel refused an Agent group added after undoing a delete: " + diagnostic);
		require(next != deleted && next.value > deleted.value,
			"An Agent group added after an undo inherited the restored identity: "
				+ std::to_string(next.value));
		require(next.value == 3,
			"The Agent group added after an undo did not continue from the mark: "
				+ std::to_string(next.value));
		resetUndoHistory();
	}

	// An empty group list says nothing about where the allocator got to, which
	// is exactly why the mark cannot be inferred from it.
	void aDocumentWithEveryAgentGroupDeletedStillKeepsTheAllocatorAhead()
	{
		core::Building building("All gone", 12, 3);
		buildWorld(building);

		std::string diagnostic;
		auto const first = building.addAgentGroup("First");
		auto const second = building.addAgentGroup("Second");
		require(building.deleteAgentGroup(first, &diagnostic),
			"Deleting the first Agent group failed: " + diagnostic);
		require(building.deleteAgentGroup(second, &diagnostic),
			"Deleting the second Agent group failed: " + diagnostic);
		require(building.getAgentGroupCount() == 0,
			"Deleting every Agent group left some behind: " + groupSummary(building));

		auto const yaml = serializeBuilding(building);
		uint64_t mark{ 0 };
		require(persistedAllocatorMark(yaml, mark) && mark == 3,
			"A save with no Agent groups left did not carry the mark: " + allocatorMarkText(yaml));

		auto const reopened = loadBuilding(yaml);
		require(reopened->getAgentGroupCount() == 0,
			"A reopened empty document came back with Agent groups: " + groupSummary(*reopened));

		auto const third = reopened->addAgentGroup("Third");
		require(third.value > second.value && third.value == 3,
			"The first Agent group after reopening an empty document did not continue "
				"from the mark: " + std::to_string(third.value));
	}

	// Sparse survivors: the allocator goes above the highest of them, not one
	// past the count of them.
	void sparseAgentGroupIdsKeepTheAllocatorAboveTheHighestLiveId()
	{
		core::Building building("Sparse", 12, 3);
		buildWorld(building);
		building.addAgentGroup("One");
		building.addAgentGroup("Two");
		auto const three = building.addAgentGroup("Three");
		std::string diagnostic;
		require(building.deleteAgentGroup(three, &diagnostic),
			"Deleting the middle Agent group failed: " + diagnostic);

		// Widen the gaps and drop the mark, so the reader has only a sparse
		// pair of survivors to go on.
		auto yaml = serializeBuilding(building);
		yaml = replacedOnce(yaml, "  - id: 1\n", "  - id: 4\n");
		yaml = replacedOnce(yaml, "  - id: 2\n", "  - id: 42\n");
		yaml = withoutLine(yaml, "nextAgentGroupId: 4\n");

		auto const loaded = loadBuilding(yaml);
		require(loaded->getAgentGroupCount() == 2,
			"The sparse document did not load both Agent groups: " + groupSummary(*loaded));
		require(loaded->getAgentGroupIds().back().value == 42,
			"The sparse document lost its highest Agent group ID: " + groupSummary(*loaded));

		auto const next = loaded->addAgentGroup("Next");
		require(next.value == 43,
			"A sparse document's allocator did not come in above the highest live ID: "
				+ std::to_string(next.value));
	}

	// The boundary the ticket found: a document whose highest ID is the top of
	// the range used to wrap the allocator onto zero.
	void aDocumentAtTheTopOfTheRangeLoadsExhaustedRatherThanWrappingToZero()
	{
		core::Building building("Top of range", 12, 3);
		buildWorld(building);
		auto const first = building.addAgentGroup("First");
		building.addAgentGroup("Last");
		std::string diagnostic;
		require(building.deleteAgentGroup(first, &diagnostic),
			"Deleting the first Agent group failed: " + diagnostic);

		// The lone survivor moved to the top of the range, with no mark in the
		// file: the worst case a pre-#123 document can present.
		auto yaml = serializeBuilding(building);
		yaml = replacedOnce(yaml, "  - id: 2\n",
			"  - id: " + std::to_string(kTopOfRange) + "\n");
		yaml = withoutLine(yaml, "nextAgentGroupId: 3\n");

		auto const loaded = loadBuilding(yaml);
		require(loaded->getAgentGroupCount() == 1,
			"The top-of-range document did not load its group: " + groupSummary(*loaded));
		require(loaded->getAgentGroupIds().front().value == kTopOfRange,
			"The top-of-range Agent group ID did not survive the load: " + groupSummary(*loaded));

		require(!loaded->canAddAgentGroup("Another", &diagnostic),
			"A Building at the top of the Agent group ID range still allowed an allocation");
		require(diagnostic.find("every Agent group ID") != std::string::npos,
			"The exhausted refusal did not say why: " + diagnostic);

		bool allocated = false;
		try
		{
			auto const id = loaded->addAgentGroup("Another");
			allocated = true;
			require(!static_cast<bool>(id),
				"An exhausted Building issued a falsey AgentGroupId: "
					+ std::to_string(id.value));
		}
		catch (std::exception const&)
		{
		}
		require(!allocated, "An exhausted Building created an Agent group anyway");
		require(loaded->getAgentGroupCount() == 1,
			"A refused Agent group was created anyway: " + groupSummary(*loaded));
		requireNoNullAgentGroupId(*loaded, "at the top of the range");

		// The exhausted state itself round-trips: the save says so, and the
		// reopen agrees rather than reading the zero as a fresh start.
		auto const roundTrip = serializeBuilding(*loaded);
		uint64_t mark{ 0 };
		require(persistedAllocatorMark(roundTrip, mark) && mark == 0,
			"An exhausted save did not record that the range is spent: "
				+ allocatorMarkText(roundTrip));

		auto const again = loadBuilding(roundTrip);
		require(again->getAgentGroupCount() == 1,
			"The reopened exhausted document lost its Agent group: " + groupSummary(*again));
		require(!again->canAddAgentGroup("Another", &diagnostic),
			"The reopened exhausted Building allowed an allocation");
		requireNoNullAgentGroupId(*again, "after reopening an exhausted document");
	}

	// A zero mark is the exhaustion marker, not a group. It must never be read
	// back as an Agent group with ID zero, and must never be handed out.
	void anExplicitZeroMarkReadsAsExhaustionNotAsAGroupId()
	{
		auto const loaded = loadBuilding(exhaustedDocument());
		require(loaded->getAgentGroupCount() == 2,
			"The zero-mark document did not load both of its Agent groups: "
				+ groupSummary(*loaded));
		requireNoNullAgentGroupId(*loaded, "loaded from a zero mark");

		std::string diagnostic;
		require(!loaded->canAddAgentGroup("Another", &diagnostic),
			"A Building marked exhausted still allowed an allocation");
		require(diagnostic.find("every Agent group ID") != std::string::npos,
			"The exhausted refusal did not say why: " + diagnostic);
		require(loaded->getAgentGroupCount() == 2,
			"A refused allocation changed the Agent group list: " + groupSummary(*loaded));
	}

	// A mark that runs backwards against the groups in the same file cannot be
	// honoured and cannot be silently clamped: it says the file contradicts
	// itself, and says both numbers.
	void aMarkThatRunsBackwardsAgainstItsGroupsIsRefused()
	{
		auto const malformed = backwardsMarkDocument();

		std::string refusal;
		require(tryLoad(malformed.yaml, refusal) == nullptr,
			"A document whose nextAgentGroupId runs backwards against its groups was accepted");
		require(refusal.find(std::to_string(malformed.mark)) != std::string::npos,
			"The refusal did not name the mark it was given: " + refusal);
		require(refusal.find(std::to_string(malformed.highestDefined)) != std::string::npos,
			"The refusal did not name the highest group the document defines: " + refusal);
	}

	// Refusing the boundary must not cost the editor its work. The mark and the
	// group list are read and judged before the Building is reset, so a refused
	// file leaves the live Building exactly where it was.
	void aRefusedMarkLeavesTheLiveBuildingAndItsGroupsAlone()
	{
		core::Building building("Live Building", 12, 3);
		buildWorld(building);
		auto const mine = building.addAgentGroup("Mine");
		auto const before = groupSummary(building);

		std::string refusal;
		require(!tryLoadInto(building, backwardsMarkDocument().yaml, refusal),
			"A malformed document was loaded into the live Building");
		require(groupSummary(building) == before,
			"A refused load disturbed the live Building's Agent groups: " + groupSummary(building));

		auto const alsoMine = building.addAgentGroup("Also mine");
		require(alsoMine.value == mine.value + 1,
			"The live Building's allocator moved on a refused load: "
				+ std::to_string(alsoMine.value));
		require(groupSummary(building) == before + "2:Also mine;",
			"The live Building did not carry on as if nothing had been attempted: "
				+ groupSummary(building));
	}

	// Documents written before the mark existed still open. They say nothing
	// about the IDs they already used up, so the reader takes the safest value
	// it can derive: one past the highest ID the file still names.
	void aDocumentWithoutTheAllocatorFieldStillLoadsAndDerivesASafeNextId()
	{
		core::Building building("Legacy", 12, 3);
		buildWorld(building);
		building.addAgentGroup("One");
		auto const two = building.addAgentGroup("Two");
		auto const yaml = withoutLine(serializeBuilding(building), "nextAgentGroupId: 3\n");

		auto const loaded = loadBuilding(yaml);
		require(loaded->getAgentGroupCount() == 2,
			"A markless document did not load its Agent groups: " + groupSummary(*loaded));
		auto const three = loaded->addAgentGroup("Three");
		require(three != two && three.value == 3,
			"A markless document did not derive a safe next ID: " + std::to_string(three.value));
	}

	// A document with no Agent group section at all - every document older
	// than version 9 - opens and starts its allocation at the beginning.
	void aDocumentWithNoAgentGroupSectionStartsTheAllocationFresh()
	{
		core::Building building("Older schema", 12, 3);
		buildWorld(building);
		building.addAgentGroup("One");
		// Neither the group section nor the mark: the shape every document older
		// than version 9 presents.
		auto yaml = withoutTopLevelBlock(serializeBuilding(building), "agentGroups");
		yaml = withoutLine(yaml, "nextAgentGroupId: 2\n");
		require(allocatorMarkText(yaml) == "<absent>",
			"The older-schema fixture still carries a mark: " + allocatorMarkText(yaml));

		auto const loaded = loadBuilding(yaml);
		require(loaded->getAgentGroupCount() == 0,
			"A document with no Agent group section came back with groups: "
				+ groupSummary(*loaded));
		auto const first = loaded->addAgentGroup("First");
		require(first.value == 1,
			"A document with no Agent group section did not start allocating at 1: "
				+ std::to_string(first.value));
	}

	// The panel's own seam: an exhausted Building refuses the add, says why,
	// and commits nothing - no group, no dirty state, no undo entry.
	void anExhaustedBuildingRefusesThePanelSeamAndCommitsNothing()
	{
		resetUndoHistory();
		auto building = loadBuilding(exhaustedDocument());
		auto const before = groupSummary(*building);
		building->markSaved();
		auto const historyBefore = gBuildingDocumentHistory.undoCount();

		std::string diagnostic;
		auto const refused = commitAgentGroupAdd(building, "Too many", diagnostic);
		require(!static_cast<bool>(refused),
			"The panel created an Agent group in an exhausted Building");
		require(!diagnostic.empty(), "The panel refused an Agent group without a diagnostic");
		require(gBuildingDocumentHistory.undoCount() == historyBefore,
			"A refused Agent group still reached the undo history");
		require(groupSummary(*building) == before,
			"A refused Agent group changed the Building: " + groupSummary(*building));
		resetUndoHistory();
	}
}

void runAgentGroupIdAllocationSmokeChecks()
{
	resetAgentGroupsPanelState();
	aFreshBuildingIssuesLiveIdsInOrder();
	theHighestDeletedAgentGroupIdIsNotReissuedAcrossASaveAndReopen();
	theSaveCarriesTheAllocatorMark();
	theHighestDeletedAgentGroupIdIsNotReissuedAcrossUndoAndRedo();
	undoingADeleteKeepsTheAllocatorAboveTheRestoredIdentity();
	aDocumentWithEveryAgentGroupDeletedStillKeepsTheAllocatorAhead();
	sparseAgentGroupIdsKeepTheAllocatorAboveTheHighestLiveId();
	aDocumentAtTheTopOfTheRangeLoadsExhaustedRatherThanWrappingToZero();
	anExplicitZeroMarkReadsAsExhaustionNotAsAGroupId();
	aMarkThatRunsBackwardsAgainstItsGroupsIsRefused();
	aRefusedMarkLeavesTheLiveBuildingAndItsGroupsAlone();
	aDocumentWithoutTheAllocatorFieldStillLoadsAndDerivesASafeNextId();
	aDocumentWithNoAgentGroupSectionStartsTheAllocationFresh();
	anExhaustedBuildingRefusesThePanelSeamAndCommitsNothing();
	resetAgentGroupsPanelState();
}
