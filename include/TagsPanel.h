#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace core
{
	class Building;
}

// The create action is available only for a Building with a saved file and no
// registry reference. Exposed separately for headless editor checks.
bool canCreateAgentTagRegistry(std::shared_ptr<const core::Building> const& building,
	std::string const& buildingFilepath, std::string* diagnostic = nullptr);

// Selection, like creation, is available only after the Building has a saved
// location and while it has no registry reference.
bool canSelectAgentTagRegistry(std::shared_ptr<const core::Building> const& building,
	std::string const& buildingFilepath, std::string* diagnostic = nullptr);

using AgentTagRegistryPathSelector
	= std::function<std::optional<std::string>()>;

// Renders attached-registry status plus create and select actions. The caller
// supplies the native-dialog callback so this panel remains headless-testable.
// Returns true after a registry was attached, allowing the caller to persist
// the changed Building immediately.
bool renderTagsPanel(std::shared_ptr<core::Building> const& building,
	std::string const& buildingFilepath,
	AgentTagRegistryPathSelector const& selectRegistryPath = {});
