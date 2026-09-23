#pragma once

// The Agents table's Group cell (ticket #110): the combobox that assigns an
// Agent to one defined Agent group, or to none.
//
// This lives in its own translation unit - as the Groups table did in #109 -
// so the headless smoke checks compile and drive the real widget inside a
// CPU-side ImGui context instead of a mirrored copy, and so the commit seam
// below is exactly the one the GUI calls. UI.cpp's spdlog/nfd dependencies
// never link headlessly.
//
// The assignment is authored editor metadata (ADR 0006). It is available
// whether or not the simulation runs, it marks the document modified, and it
// has no effect on movement, pathfinding, capacity, traversal coordination,
// runtime snapshots, or simulation events.

#include <memory>
#include <string>

#include "core/EntityId.h"

namespace core
{
	class World;
}

// Assigns `agent` to `group`, or clears the assignment when `group` is the
// empty AgentGroupId, and commits exactly one undoable document edit. A
// refused operation - an Agent or an Agent group this World never issued -
// changes nothing, leaves the undo stack untouched, and reports the reason
// through `diagnostic`.
bool commitAgentGroupAssignment(
	std::shared_ptr<core::World> const& world,
	core::AgentId agent, core::AgentGroupId group, std::string& diagnostic);

// The label a Group cell shows for an Agent's current assignment: the
// assigned Agent group's current name read through the World, or `<none>`
// when the Agent holds no Agent group. Because the name is looked up rather
// than copied, a rename is what the cell shows the next time it is drawn.
std::string agentGroupAssignmentLabel(core::World const& world, core::AgentId agent);

// Renders one Group cell: `<none>` first, then every defined Agent group in
// creation order. Re-choosing the group an Agent already holds is not an
// edit and commits nothing. Names are drawn as literal display text, never as
// widget labels, so a name carrying "##" - which ImGui would otherwise read
// as the start of an invisible ID suffix - still shows whole (#124).
void renderAgentGroupAssignmentCell(
	std::shared_ptr<core::World> const& world, core::AgentId agent);
