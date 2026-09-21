// Building snapshot integration; see include/DocumentEdit.h.

#include "DocumentEdit.h"

#include <exception>
#include <utility>

#include "core/Building.h"
#include "core/Log.h"
#include "core/YamlSerializer.h"

using namespace std;

DocumentHistory gBuildingDocumentHistory;

optional<DocumentSnapshot> captureDocumentSnapshot(
	shared_ptr<const core::Building> const& building, DocumentHistory const& history)
{
	if (!building) return nullopt;
	try
	{
		auto serializer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		building->serialize(*serializer, workData);
		serializer->serialize();
		return history.capture(serializer->getSerializedString());
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Undo", 0, core::LogLevel::Error,
			"Could not capture editor state: " + string(error.what()));
		return nullopt;
	}
}

void commitDocumentEdit(optional<DocumentSnapshot> snapshot, DocumentHistory& history)
{
	history.commit(std::move(snapshot));
}
