#pragma once

#include <memory>
#include <string>

#include "core/AgentBehaviour.h"
#include "core/EntityId.h"

namespace core { class Building; }

// Public editor transaction seams: validation happens in Building before the
// captured snapshot is committed, so every successful assignment/configuration
// replacement is one undoable edit and every refusal leaves history untouched.
bool commitAgentBehaviourAssignment(
	std::shared_ptr<core::Building> const& building, core::AgentId agent,
	core::AgentBehaviourId behaviour, uint64_t revision,
	core::AgentBehaviourConfiguration const& configuration,
	std::string& diagnostic);
bool commitAgentBehaviourClear(std::shared_ptr<core::Building> const& building,
	core::AgentId agent, std::string& diagnostic);

// Compact picker for the Agents table and full schema-generated editor for the
// selected-Agent panel. Marker values are always displayed and chosen by name.
void renderAgentBehaviourAssignmentCell(
	std::shared_ptr<core::Building> const& building, core::AgentId agent);
void renderAgentBehaviourConfigurationPanel(
	std::shared_ptr<core::Building> const& building, core::AgentId agent);
