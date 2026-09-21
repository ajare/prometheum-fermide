#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "DocumentHistory.h"
#include "core/EntityId.h"

namespace core
{
	class AgentTagRegistry;
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

// Registry edits use a history that is separate from the Building history.
// The same shared registry instance always resolves to the same history.
DocumentHistory& agentTagRegistryDocumentHistory(
	std::shared_ptr<core::AgentTagRegistry> const& registry);

core::AgentTagId commitAgentTagAdd(
	std::shared_ptr<core::AgentTagRegistry> const& registry,
	std::string const& name, std::string& diagnostic);
bool commitAgentTagRename(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string const& name, std::string& diagnostic);

// Deletion is not presented by this ticket's panel, but the registry operation
// and editor transaction are available so ID non-reuse is enforceable and
// testable before the confirmed deletion workflow is added.
bool commitAgentTagDelete(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string& diagnostic);

bool restoreAgentTagRegistrySnapshot(
	std::shared_ptr<core::AgentTagRegistry> const& registry, bool redo,
	std::string* diagnostic = nullptr);
bool saveAgentTagRegistry(
	std::shared_ptr<core::AgentTagRegistry> const& registry,
	std::string const& filepath, std::string* diagnostic = nullptr);
bool agentTagRegistryIsModified(
	std::shared_ptr<core::AgentTagRegistry> const& registry);
bool attachedAgentTagRegistryIsModified(
	std::shared_ptr<const core::Building> const& building);

// Drop transient editors and, when a document is closed or discarded, its
// saved-state/undo bookkeeping. The registry object itself remains owned by
// any other Building that shares it.
void resetTagsPanelState();
void forgetAgentTagRegistryDocument(
	std::shared_ptr<core::AgentTagRegistry> const& registry);

// Renders attached-registry status, independent save/undo controls, an
// alphabetical create/rename editor, and create/select attachment actions.
// Returns true after a registry was attached, allowing the caller to persist
// the changed Building immediately.
bool renderTagsPanel(std::shared_ptr<core::Building> const& building,
	std::string const& buildingFilepath,
	AgentTagRegistryPathSelector const& selectRegistryPath = {});
