#include "DocumentHistory.h"

#include <utility>

using namespace std;

DocumentSnapshot DocumentHistory::capture(string serializedDocument) const
{
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
	return !mSavedStateId || mCurrentStateId != *mSavedStateId;
}
