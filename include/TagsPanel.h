#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "DocumentHistory.h"
#include "core/AgentTag.h"
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
bool commitAgentTagColourAdd(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string& diagnostic);
bool commitAgentTagColourEdit(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	core::AgentColour colour, std::string& diagnostic);
bool commitAgentTagColourRemove(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string& diagnostic);
bool commitAgentTagWalkSpeedModifierAdd(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string& diagnostic);
bool commitAgentTagWalkSpeedModifierEdit(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	core::AgentModifierRange range, std::string& diagnostic);
bool commitAgentTagWalkSpeedModifierRemove(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string& diagnostic);

// Deletes a tag and every assignment in loaded dependent Buildings as one
// registry-history transaction. A refusal changes neither registry nor
// Building state and creates no history entry.
bool commitAgentTagDelete(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string& diagnostic);

// Case-insensitive name filtering used by the Tags panel. The visible '#'
// prefix participates in matching without becoming part of the stored name.
bool agentTagNameMatchesFilter(std::string const& name, std::string const& filter);
uint64_t loadedAgentTagUsageCount(core::AgentTagRegistry const& registry,
	core::AgentTagId id);

// Used tags are confirmed; unused tags delete immediately. Every confirmation
// reports aggregate loaded usage and warns that closed Buildings are unknown.
bool agentTagDeleteRequiresConfirmation(core::AgentTagRegistry const& registry,
	core::AgentTagId id);
std::string agentTagDeleteConfirmationText(core::AgentTagRegistry const& registry,
	core::AgentTagId id);
void requestAgentTagDelete(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id);
bool agentTagDeletePending(core::AgentTagId* id = nullptr,
	uint64_t* loadedAgentCount = nullptr);
bool confirmPendingAgentTagDelete(
	std::shared_ptr<core::AgentTagRegistry> const& registry,
	std::string& diagnostic);
void cancelPendingAgentTagDelete();

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
