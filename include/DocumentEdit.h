#pragma once

// World snapshot integration for the reusable per-document history. The GUI
// currently owns one World document; external editor documents can own
// separate DocumentHistory instances without sharing stacks or saved state.

#include <memory>
#include <optional>

#include "DocumentHistory.h"

namespace core
{
	class World;
}

extern DocumentHistory gWorldDocumentHistory;

std::optional<DocumentSnapshot> captureDocumentSnapshot(
	std::shared_ptr<const core::World> const& world,
	DocumentHistory const& history = gWorldDocumentHistory);
void commitDocumentEdit(std::optional<DocumentSnapshot> snapshot,
	DocumentHistory& history = gWorldDocumentHistory);
