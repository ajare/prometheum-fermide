#pragma once

// Reusable history and saved-state tracking for independently persisted editor
// documents. The history owns only serialized document snapshots, so World
// documents and future external documents can each keep an isolated instance.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// Optional document-specific state lets an editor transaction coordinate
// dependent documents while the reusable history remains unaware of their
// concrete types. Ordinary World snapshots leave this empty.
struct DocumentSnapshotContext
{
	virtual ~DocumentSnapshotContext() = default;

	// Coordinated snapshots may depend on other live documents. Once one of
	// those documents closes, crossing that history entry could only restore a
	// partial transaction, so the owning history must discard it.
	virtual bool isRestorable() const { return true; }
};

struct DocumentSnapshot
{
	std::string yaml;
	uint64_t stateId{ 0 };
	std::shared_ptr<DocumentSnapshotContext> context;
};

class DocumentHistory
{
public:
	static constexpr size_t MaximumEntries{ 100 };
	using RestoreDocument = std::function<bool(DocumentSnapshot const&)>;

	DocumentSnapshot capture(std::string serializedDocument) const;
	void commit(std::optional<DocumentSnapshot> snapshot);

	// The callback restores the selected snapshot before the stacks are changed.
	// Returning false or throwing therefore leaves this history untouched.
	bool undo(std::optional<DocumentSnapshot> current, RestoreDocument const& restoreDocument);
	bool redo(std::optional<DocumentSnapshot> current, RestoreDocument const& restoreDocument);

	void clear();
	void markSaved();

	bool isModified() const;
	bool canUndo() const;
	bool canRedo() const;
	size_t undoCount() const;
	size_t redoCount() const;
	uint64_t currentStateId() const { return mCurrentStateId; }

	// Read-only access supports diagnostics and snapshot-focused smoke checks
	// without allowing callers to splice one document's commands into another.
	std::deque<DocumentSnapshot> const& undoEntries() const;
	std::deque<DocumentSnapshot> const& redoEntries() const;

private:
	bool restore(std::optional<DocumentSnapshot> current, RestoreDocument const& restoreDocument,
		bool redo);
	static void append(std::deque<DocumentSnapshot>& history, DocumentSnapshot snapshot);
	void discardUnrestorableEntries() const;
	static void discardUnrestorableEntries(std::deque<DocumentSnapshot>& history);

	mutable std::deque<DocumentSnapshot> mUndo;
	mutable std::deque<DocumentSnapshot> mRedo;
	uint64_t mCurrentStateId{ 0 };
	uint64_t mNextStateId{ 1 };
	std::optional<uint64_t> mSavedStateId;
};
