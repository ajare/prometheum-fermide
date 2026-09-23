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
		DocumentHistory worldHistory;
		DocumentHistory registryHistory;

		require(worldHistory.isModified() && registryHistory.isModified(),
			"A new unsaved history was reported clean");
		worldHistory.markSaved();
		registryHistory.markSaved();
		require(!worldHistory.isModified() && !registryHistory.isModified(),
			"Marking fresh histories saved did not make them clean");

		worldHistory.commit(worldHistory.capture("world 0"));
		require(worldHistory.canUndo() && !worldHistory.canRedo(),
			"A World edit did not produce exactly an undo command");
		require(worldHistory.isModified(), "A World edit did not dirty its history");
		require(!registryHistory.canUndo() && !registryHistory.canRedo()
			&& !registryHistory.isModified(),
			"A World edit leaked into the independent registry history");

		std::string restoredWorld;
		auto restoreWorld = [&restoredWorld](DocumentSnapshot const& target)
		{
			restoredWorld = target.yaml;
			return true;
		};
		require(worldHistory.undo(worldHistory.capture("world 1"), restoreWorld),
			"Undoing the first World edit failed");
		require(restoredWorld == "world 0", "Undo restored the wrong World state");
		require(!worldHistory.isModified(),
			"Undoing the first World edit did not return to the saved state");
		require(worldHistory.redo(worldHistory.capture("world 0"), restoreWorld),
			"Redoing the first World edit failed");
		require(restoredWorld == "world 1", "Redo restored the wrong World state");
		require(worldHistory.isModified(),
			"Redoing the first World edit did not return to a dirty state");

		worldHistory.markSaved();
		require(!worldHistory.isModified(), "Saving the edited World left it dirty");
		worldHistory.commit(worldHistory.capture("world 1"));
		require(worldHistory.isModified(), "A post-save World edit stayed clean");

		require(worldHistory.undo(worldHistory.capture("world 2"), restoreWorld),
			"Undoing the post-save World edit failed");
		require(restoredWorld == "world 1", "Post-save undo restored the wrong state");
		require(!worldHistory.isModified(),
			"Undoing to the saved World state did not make it clean");
		require(worldHistory.redo(worldHistory.capture("world 1"), restoreWorld),
			"Redoing the post-save World edit failed");
		require(restoredWorld == "world 2", "Post-save redo restored the wrong state");
		require(worldHistory.isModified(),
			"Redoing away from the saved World state did not make it dirty");

		auto const worldUndoCount = worldHistory.undoCount();
		auto const worldRedoCount = worldHistory.redoCount();
		auto const worldState = worldHistory.currentStateId();
		registryHistory.commit(registryHistory.capture("registry 0"));
		require(registryHistory.canUndo() && registryHistory.isModified(),
			"A registry edit did not affect its own history");
		require(worldHistory.undoCount() == worldUndoCount
			&& worldHistory.redoCount() == worldRedoCount
			&& worldHistory.currentStateId() == worldState,
			"A registry edit changed the World history");

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
