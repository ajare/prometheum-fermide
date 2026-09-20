#pragma once

// Document-level undo/redo edit state, shared between the GUI and the headless
// smoke checks. Ticket #99: the Door panel moved into DoorPanel.cpp is compiled
// into both the GUI executable and the headless binary, and it commits its
// edits through captureDocumentSnapshot()/commitDocumentEdit(); those, and the
// history they append to, live here rather than in UI.cpp's anonymous namespace
// so both links see the same definitions.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>

namespace core
{
	class Building;
}

struct DocumentSnapshot
{
	std::string yaml;
	uint64_t stateId{ 0 };
};

constexpr size_t MaximumUndoHistory{ 100 };
extern std::deque<DocumentSnapshot> gUndoHistory;
extern std::deque<DocumentSnapshot> gRedoHistory;
extern uint64_t gCurrentStateId;
extern uint64_t gNextStateId;
extern std::optional<uint64_t> gSavedStateId;

std::optional<DocumentSnapshot> captureDocumentSnapshot(
	std::shared_ptr<const core::Building> const& building);
void commitDocumentEdit(std::optional<DocumentSnapshot> snapshot);
