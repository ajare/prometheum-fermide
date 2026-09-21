#pragma once

#include <memory>
#include <string>

namespace core
{
	class Building;
}

// The create action is available only for a Building with a saved file and no
// registry reference. Exposed separately for headless editor checks.
bool canCreateAgentTagRegistry(std::shared_ptr<const core::Building> const& building,
	std::string const& buildingFilepath, std::string* diagnostic = nullptr);

// Renders attached-registry status and the empty-registry creation action.
// Returns true after a registry was created and attached, allowing the caller
// to persist the changed Building immediately.
bool renderTagsPanel(std::shared_ptr<core::Building> const& building,
	std::string const& buildingFilepath);
