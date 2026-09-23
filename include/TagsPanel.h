#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "DocumentHistory.h"
#include "core/AgentTag.h"
#include "core/EntityId.h"

namespace core
{
	class AgentTagRegistry;
	class World;
}

// The create action is available only for a World with a saved file and no
// registry reference. Exposed separately for headless editor checks.
bool canCreateAgentTagRegistry(std::shared_ptr<const core::World> const& world,
	std::string const& worldFilepath, std::string* diagnostic = nullptr);

// Selection requires a saved World location. It attaches an initial registry
// or switches directly when there are no assignments.
bool canSelectAgentTagRegistry(std::shared_ptr<const core::World> const& world,
	std::string const& worldFilepath, std::string* diagnostic = nullptr);

// World-reference edits own World undo entries, never registry-history
// entries. Direct switching is refused while assignments exist. The clearing
// variants are reserved for an explicitly confirmed destructive action.
bool commitAgentTagRegistryDetach(
	std::shared_ptr<core::World> const& world, std::string& diagnostic);
bool commitAgentTagRegistryDetachClearingAssignments(
	std::shared_ptr<core::World> const& world, std::string& diagnostic);
bool commitAgentTagRegistrySwitch(
	std::shared_ptr<core::World> const& world,
	std::string const& worldFilepath, std::string const& registryFilepath,
	std::string& diagnostic);
bool commitAgentTagRegistrySwitchClearingAssignments(
	std::shared_ptr<core::World> const& world,
	std::string const& worldFilepath, std::string const& registryFilepath,
	std::string& diagnostic);

// Confirmation is state-free until confirm: requesting and cancelling change no
// document and create no undo entry. The exposed text lists everything confirm
// will clear and the resulting detach or replacement attachment.
void requestAgentTagRegistryDetach(
	std::shared_ptr<core::World> const& world);
void requestAgentTagRegistrySwitch(
	std::shared_ptr<core::World> const& world,
	std::string worldFilepath, std::string registryFilepath);
bool agentTagRegistryChangePending(std::string* consequence = nullptr);
bool confirmPendingAgentTagRegistryChange(std::string& diagnostic);
void cancelPendingAgentTagRegistryChange();

using AgentTagRegistryPathSelector
	= std::function<std::optional<std::string>()>;

// Registry edits use a history that is separate from the World history.
// The same shared registry instance always resolves to the same history.
DocumentHistory& agentTagRegistryDocumentHistory(
	std::shared_ptr<core::AgentTagRegistry> const& registry);

core::AgentTagId commitAgentTagAdd(
	std::shared_ptr<core::AgentTagRegistry> const& registry,
	std::string const& name, std::string& diagnostic);
bool commitAgentTagRename(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string const& name, std::string& diagnostic);

// The intrinsic tag display Colour (chip colour) commits through the same
// registry history as every other definition edit. It never affects Agents.
bool commitAgentTagDisplayColourEdit(
	std::shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, core::AgentColour colour, std::string& diagnostic);
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
bool commitAgentTagHeightModifierAdd(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string& diagnostic);
bool commitAgentTagHeightModifierEdit(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	core::AgentModifierRange range, std::string& diagnostic);
bool commitAgentTagHeightModifierRemove(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string& diagnostic);

// Deletes a tag and every assignment in loaded dependent Worlds as one
// registry-history transaction. A refusal changes neither registry nor
// World state and creates no history entry.
bool commitAgentTagDelete(
	std::shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id,
	std::string& diagnostic);

// Case-insensitive name filtering used by the Tags panel. The visible '#'
// prefix participates in matching without becoming part of the stored name.
bool agentTagNameMatchesFilter(std::string const& name, std::string const& filter);
uint64_t loadedAgentTagUsageCount(core::AgentTagRegistry const& registry,
	core::AgentTagId id);

// Used tags are confirmed; unused tags delete immediately. Every confirmation
// reports aggregate loaded usage and warns that closed Worlds are unknown.
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
// Reload refuses dirty state, validates all loaded dependent Worlds before
// committing, and clears history only after the transactional core reload.
bool reloadAgentTagRegistry(
	std::shared_ptr<core::AgentTagRegistry> const& registry,
	std::string const& filepath, std::string* diagnostic = nullptr);
bool saveAgentTagRegistry(
	std::shared_ptr<core::AgentTagRegistry> const& registry,
	std::string const& filepath, std::string* diagnostic = nullptr);
bool agentTagRegistryIsModified(
	std::shared_ptr<core::AgentTagRegistry> const& registry);
bool attachedAgentTagRegistryIsModified(
	std::shared_ptr<const core::World> const& world);

// A World save target carries the separately persisted dependency path and
// each editor document's own saved-state marker. During Save As, the explicit
// registry path identifies the source directory; a different World target
// directory receives an independent adjacent registry copy.
struct WorldDocumentSaveTarget
{
	std::shared_ptr<core::World> world;
	std::string worldFilepath;
	std::string registryFilepath;
	DocumentHistory* worldHistory{ nullptr };
	// Source package path for the separately persisted Agent behaviour registry.
	std::string behaviourPackagePath;

	WorldDocumentSaveTarget() = default;
	WorldDocumentSaveTarget(std::shared_ptr<core::World> value,
		std::string worldPath, std::string tagRegistryPath,
		DocumentHistory* history, std::string behaviourPath = {})
		: world(std::move(value)), worldFilepath(std::move(worldPath)),
		  registryFilepath(std::move(tagRegistryPath)), worldHistory(history),
		  behaviourPackagePath(std::move(behaviourPath))
	{
	}
};

// Save always writes a dirty attached registry before the requested World.
// Save All deduplicates shared registries, writes every dirty registry first,
// and only then writes dirty Worlds. Any registry failure leaves every
// World untouched; successful documents advance only their own markers.
bool saveWorldDocument(WorldDocumentSaveTarget const& target,
	std::string* diagnostic = nullptr);
bool saveAllDocuments(std::vector<WorldDocumentSaveTarget> const& targets,
	std::string* diagnostic = nullptr);

// Close/exit confirmation text lists each independently dirty document rather
// than describing an attached but clean registry as unsaved.
std::string unsavedDocumentPromptText(WorldDocumentSaveTarget const& target);

// Drop transient editors and, when a document is closed or discarded, its
// saved-state/undo bookkeeping. The registry object itself remains owned by
// any other World that shares it.
void resetTagsPanelState();
void forgetAgentTagRegistryDocument(
	std::shared_ptr<core::AgentTagRegistry> const& registry);

// Renders attached-registry status, independent save/undo controls, an
// alphabetical stack of per-tag sections with rename/delete/property
// controls, and create/select/detach/switch actions.
// Returns true after the World reference changed, allowing the caller to
// persist the changed World immediately.
bool renderTagsPanel(std::shared_ptr<core::World> const& world,
	std::string const& worldFilepath,
	AgentTagRegistryPathSelector const& selectRegistryPath = {});
