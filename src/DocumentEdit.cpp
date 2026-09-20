// Document-level undo/redo edit state; see include/DocumentEdit.h.

#include "DocumentEdit.h"

#include <exception>
#include <utility>

#include "core/Building.h"
#include "core/Log.h"
#include "core/YamlSerializer.h"

using namespace std;

deque<DocumentSnapshot> gUndoHistory;
deque<DocumentSnapshot> gRedoHistory;
uint64_t gCurrentStateId{ 0 };
uint64_t gNextStateId{ 1 };
optional<uint64_t> gSavedStateId;

optional<DocumentSnapshot> captureDocumentSnapshot(
	shared_ptr<const core::Building> const& building)
{
	if (!building) return nullopt;
	try
	{
		auto serializer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		building->serialize(*serializer, workData);
		serializer->serialize();
		return DocumentSnapshot{ serializer->getSerializedString(), gCurrentStateId };
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Undo", 0, core::LogLevel::Error,
			"Could not capture editor state: " + string(error.what()));
		return nullopt;
	}
}

void commitDocumentEdit(optional<DocumentSnapshot> snapshot)
{
	if (!snapshot) return;
	gUndoHistory.push_back(std::move(*snapshot));
	if (gUndoHistory.size() > MaximumUndoHistory) gUndoHistory.pop_front();
	gRedoHistory.clear();
	gCurrentStateId = gNextStateId++;
}
