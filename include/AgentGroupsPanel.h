#pragma once

// The Agents section's Agent-group table (ticket #109). This lives in its own
// translation unit - rather than inside UI.cpp - so the headless smoke checks
// can compile and render the real panel inside a CPU-side ImGui context;
// UI.cpp's spdlog/nfd dependencies never link headlessly. It is the same seam
// ticket #99 opened for the Door panel.
//
// The commit functions below are the panel's own boundary: they are exactly
// what the widgets call, so a headless check exercises the real add/rename
// path - snapshot, validation, history - rather than a mirrored copy.

#include <memory>
#include <string>

#include "core/EntityId.h"

namespace core
{
	class Building;
}

// Creates a group named `rawName` (trimmed by the Building) and commits one
// undoable document edit. Returns the new AgentGroupId, or a falsy ID with
// `diagnostic` naming the refused name's problem and no history entry.
core::AgentGroupId commitAgentGroupAdd(
	std::shared_ptr<core::Building> const& building,
	std::string const& rawName, std::string& diagnostic);

// Renames an existing group in place, keeping its ID and its position in the
// creation order. Commits one undoable document edit on success; a refused
// rename changes nothing and leaves the history untouched.
bool commitAgentGroupRename(
	std::shared_ptr<core::Building> const& building, core::AgentGroupId id,
	std::string const& rawName, std::string& diagnostic);

// Drops every in-progress inline edit. Called when the document is replaced -
// new, opened, or restored by undo - so a stale editor never resumes against
// a different Building's group.
void resetAgentGroupsPanelState();

// Renders the Groups table for the Agents collapsible section: one inline
// rename editor per group, in creation order, plus the add row. Available
// whether or not the simulation is running. This slice exposes no delete.
void renderAgentGroupsPanel(std::shared_ptr<core::Building> const& building);
