#pragma once

// The selected Agent's searchable Agent tag checklist (ticket #131). The
// Building remains the sole assignment mutation boundary; this panel only
// captures one pre-edit snapshot, asks the Building to assign or remove one
// stable tag ID, and commits one Building-history entry on success.

#include <memory>
#include <string>

#include "core/EntityId.h"

namespace core
{
	class Building;
}

// Moves one Agent/tag association to `assigned`. Re-assigning an already
// assigned tag and removing an unassigned tag are refused by Building, as are
// unknown IDs, absent registries, and edits while simulation is running. A
// refusal mutates neither the Building nor its undo history.
bool commitAgentTagAssignment(
	std::shared_ptr<core::Building> const& building,
	core::AgentId agent, core::AgentTagId tag, bool assigned,
	std::string& diagnostic);

// Reports the selected Agent's effective Colour and sampled Walk speed
// modifier, including source tags or their respective defaults. Kept separate
// so the headless ImGui seam exercises the Selection panel's read-only content.
void renderAgentEffectiveProperties(
	std::shared_ptr<core::Building> const& building, core::AgentId agent);

// Renders an alphabetical, searchable checklist of every tag in the attached
// registry. Assigned rows use the same checkbox and remain removable while
// paused. Conflicting unassigned tags remain visible but disabled with the
// core validation diagnostic. All assignment edits are disabled while running.
void renderAgentTagAssignmentChecklist(
	std::shared_ptr<core::Building> const& building, core::AgentId agent);

// Clears the transient search whenever the selected Building document is
// closed, replaced, or restored through undo.
void resetAgentTagAssignmentPanelState();
