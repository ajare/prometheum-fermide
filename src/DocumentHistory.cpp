#include "DocumentHistory.h"

#include <iterator>
#include <utility>

using namespace std;

DocumentSnapshot DocumentHistory::capture(string serializedDocument) const
{
	discardUnrestorableEntries();
	return { std::move(serializedDocument), mCurrentStateId, nullptr };
}

void DocumentHistory::append(deque<DocumentSnapshot>& history, DocumentSnapshot snapshot)
{
	history.push_back(std::move(snapshot));
	if (history.size() > MaximumEntries) history.pop_front();
}

void DocumentHistory::commit(optional<DocumentSnapshot> snapshot)
{
	if (!snapshot) return;
	discardUnrestorableEntries();
	append(mUndo, std::move(*snapshot));
	mRedo.clear();
	mCurrentStateId = mNextStateId++;
}

bool DocumentHistory::undo(optional<DocumentSnapshot> current,
	RestoreDocument const& restoreDocument)
{
	return restore(std::move(current), restoreDocument, false);
}

bool DocumentHistory::redo(optional<DocumentSnapshot> current,
	RestoreDocument const& restoreDocument)
{
	return restore(std::move(current), restoreDocument, true);
}

bool DocumentHistory::restore(optional<DocumentSnapshot> current,
	RestoreDocument const& restoreDocument, bool redo)
{
	discardUnrestorableEntries();
	auto& source = redo ? mRedo : mUndo;
	auto& destination = redo ? mUndo : mRedo;
	if (!current || source.empty() || !restoreDocument) return false;

	auto const& target = source.back();
	if (!restoreDocument(target)) return false;

	append(destination, std::move(*current));
	mCurrentStateId = target.stateId;
	source.pop_back();
	return true;
}

void DocumentHistory::discardUnrestorableEntries(
	deque<DocumentSnapshot>& history)
{
	// Entries after the newest invalid snapshot are still contiguous with the
	// current state and remain usable. Older entries cannot be reached without
	// crossing the incomplete transaction, so discard that whole prefix rather
	// than turning several edits into one accidental history jump.
	auto newestInvalid = history.end();
	for (auto entry = history.begin(); entry != history.end(); ++entry)
		if (entry->context && !entry->context->isRestorable()) newestInvalid = entry;
	if (newestInvalid != history.end()) history.erase(history.begin(), next(newestInvalid));
}

void DocumentHistory::discardUnrestorableEntries() const
{
	discardUnrestorableEntries(mUndo);
	discardUnrestorableEntries(mRedo);
}

void DocumentHistory::clear()
{
	mUndo.clear();
	mRedo.clear();
	mCurrentStateId = 0;
	mNextStateId = 1;
	mSavedStateId.reset();
}

void DocumentHistory::markSaved()
{
	mSavedStateId = mCurrentStateId;
}

bool DocumentHistory::isModified() const
{
	discardUnrestorableEntries();
	return !mSavedStateId || mCurrentStateId != *mSavedStateId;
}

bool DocumentHistory::canUndo() const
{
	discardUnrestorableEntries();
	return !mUndo.empty();
}

bool DocumentHistory::canRedo() const
{
	discardUnrestorableEntries();
	return !mRedo.empty();
}

size_t DocumentHistory::undoCount() const
{
	discardUnrestorableEntries();
	return mUndo.size();
}

size_t DocumentHistory::redoCount() const
{
	discardUnrestorableEntries();
	return mRedo.size();
}

deque<DocumentSnapshot> const& DocumentHistory::undoEntries() const
{
	discardUnrestorableEntries();
	return mUndo;
}

deque<DocumentSnapshot> const& DocumentHistory::redoEntries() const
{
	discardUnrestorableEntries();
	return mRedo;
}
