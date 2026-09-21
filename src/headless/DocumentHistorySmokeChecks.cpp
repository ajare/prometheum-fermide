// Independent editor history, dirty-state, and saved-state checks for #127.

#include "DocumentHistory.h"

#include <stdexcept>
#include <string>

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void independentHistoriesDoNotLeakCommandsOrState()
	{
		DocumentHistory buildingHistory;
		DocumentHistory registryHistory;

		require(buildingHistory.isModified() && registryHistory.isModified(),
			"A new unsaved history was reported clean");
		buildingHistory.markSaved();
		registryHistory.markSaved();
		require(!buildingHistory.isModified() && !registryHistory.isModified(),
			"Marking fresh histories saved did not make them clean");

		buildingHistory.commit(buildingHistory.capture("building 0"));
		require(buildingHistory.canUndo() && !buildingHistory.canRedo(),
			"A Building edit did not produce exactly an undo command");
		require(buildingHistory.isModified(), "A Building edit did not dirty its history");
		require(!registryHistory.canUndo() && !registryHistory.canRedo()
			&& !registryHistory.isModified(),
			"A Building edit leaked into the independent registry history");

		std::string restoredBuilding;
		auto restoreBuilding = [&restoredBuilding](DocumentSnapshot const& target)
		{
			restoredBuilding = target.yaml;
			return true;
		};
		require(buildingHistory.undo(buildingHistory.capture("building 1"), restoreBuilding),
			"Undoing the first Building edit failed");
		require(restoredBuilding == "building 0", "Undo restored the wrong Building state");
		require(!buildingHistory.isModified(),
			"Undoing the first Building edit did not return to the saved state");
		require(buildingHistory.redo(buildingHistory.capture("building 0"), restoreBuilding),
			"Redoing the first Building edit failed");
		require(restoredBuilding == "building 1", "Redo restored the wrong Building state");
		require(buildingHistory.isModified(),
			"Redoing the first Building edit did not return to a dirty state");

		buildingHistory.markSaved();
		require(!buildingHistory.isModified(), "Saving the edited Building left it dirty");
		buildingHistory.commit(buildingHistory.capture("building 1"));
		require(buildingHistory.isModified(), "A post-save Building edit stayed clean");

		require(buildingHistory.undo(buildingHistory.capture("building 2"), restoreBuilding),
			"Undoing the post-save Building edit failed");
		require(restoredBuilding == "building 1", "Post-save undo restored the wrong state");
		require(!buildingHistory.isModified(),
			"Undoing to the saved Building state did not make it clean");
		require(buildingHistory.redo(buildingHistory.capture("building 1"), restoreBuilding),
			"Redoing the post-save Building edit failed");
		require(restoredBuilding == "building 2", "Post-save redo restored the wrong state");
		require(buildingHistory.isModified(),
			"Redoing away from the saved Building state did not make it dirty");

		auto const buildingUndoCount = buildingHistory.undoCount();
		auto const buildingRedoCount = buildingHistory.redoCount();
		auto const buildingState = buildingHistory.currentStateId();
		registryHistory.commit(registryHistory.capture("registry 0"));
		require(registryHistory.canUndo() && registryHistory.isModified(),
			"A registry edit did not affect its own history");
		require(buildingHistory.undoCount() == buildingUndoCount
			&& buildingHistory.redoCount() == buildingRedoCount
			&& buildingHistory.currentStateId() == buildingState,
			"A registry edit changed the Building history");

		auto const registryUndoCount = registryHistory.undoCount();
		auto const registryState = registryHistory.currentStateId();
		require(!registryHistory.undo(registryHistory.capture("registry 1"),
			[](DocumentSnapshot const&) { return false; }),
			"A refused document restore reported success");
		require(registryHistory.undoCount() == registryUndoCount
			&& !registryHistory.canRedo()
			&& registryHistory.currentStateId() == registryState,
			"A refused document restore mutated its history");
	}
}

void runDocumentHistorySmokeChecks()
{
	independentHistoriesDoNotLeakCommandsOrState();
}
