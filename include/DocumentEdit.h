#pragma once

// Building snapshot integration for the reusable per-document history. The GUI
// currently owns one Building document; external editor documents can own
// separate DocumentHistory instances without sharing stacks or saved state.

#include <memory>
#include <optional>

#include "DocumentHistory.h"

namespace core
{
	class Building;
}

extern DocumentHistory gBuildingDocumentHistory;

std::optional<DocumentSnapshot> captureDocumentSnapshot(
	std::shared_ptr<const core::Building> const& building,
	DocumentHistory const& history = gBuildingDocumentHistory);
void commitDocumentEdit(std::optional<DocumentSnapshot> snapshot,
	DocumentHistory& history = gBuildingDocumentHistory);
