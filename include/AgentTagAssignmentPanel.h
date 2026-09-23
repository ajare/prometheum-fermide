#pragma once

// The selected Agent's assigned Agent tags drawn as coloured chips with an
// add-tag combo (ticket #131). The World remains the sole assignment
// mutation boundary; this panel only captures one pre-edit snapshot, asks the
// World to assign or remove one stable tag ID, and commits one
// World-history entry on success.

#include <memory>
#include <string>

#include "core/EntityId.h"

namespace core
{
	class World;
}

// Moves one Agent/tag association to `assigned`. Re-assigning an already
// assigned tag and removing an unassigned tag are refused by World, as are
// unknown IDs, absent registries, and edits while simulation is running. A
// refusal mutates neither the World nor its undo history.
bool commitAgentTagAssignment(
	std::shared_ptr<core::World> const& world,
	core::AgentId agent, core::AgentTagId tag, bool assigned,
	std::string& diagnostic);

// Reports the selected Agent's effective Colour, sampled Walk speed modifier,
// and sampled Height modifier, including source tags or their defaults. Kept separate
// so the headless ImGui seam exercises the Selection panel's read-only content.
void renderAgentEffectiveProperties(
	std::shared_ptr<core::World> const& world, core::AgentId agent);

// Renders only the tags the selected Agent has as a wrapping group of
// coloured chips (tag Colour property when present, neutral grey otherwise),
// each showing just its name. Clicking a chip selects it; pressing Delete
// while the panel is focused removes the selected tag. An "Add tag..." combo
// below lists every unassigned tag in the attached registry; conflicting tags
// remain visible but disabled with the core validation diagnostic. All
// assignment edits are disabled while the simulation is running.
void renderAgentTagAssignmentChecklist(
	std::shared_ptr<core::World> const& world, core::AgentId agent);

// Clears the transient chip selection whenever the selected World document
// is closed, replaced, or restored through undo.
void resetAgentTagAssignmentPanelState();
