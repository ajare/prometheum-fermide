#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "DocumentHistory.h"
#include "core/EntityId.h"

namespace core
{
	class AgentBehaviourRegistry;
	class Building;
}

// The Behaviours panel inspects the external Agent behaviour registry package
// a Building references. Definitions are authored in the package itself; this
// panel never edits Lua source or starts Agent callbacks. It creates, selects,
// attaches, detaches, and reloads packages through the managed document seam
// and displays registry identity, protected preflight status, and diagnostics.

// The create action is available only for a Building with a saved file and no
// registry reference. Exposed separately for headless editor checks.
bool canCreateAgentBehaviourRegistry(
	std::shared_ptr<const core::Building> const& building,
	std::string const& buildingFilepath, std::string* diagnostic = nullptr);

// Selection requires a saved Building location. It attaches an initial
// registry or switches the reference directly.
bool canSelectAgentBehaviourRegistry(
	std::shared_ptr<const core::Building> const& building,
	std::string const& buildingFilepath, std::string* diagnostic = nullptr);

// Building-reference edits own Building undo entries. Direct detach/switch is
// non-destructive and refuses a used namespace. The clearing variants are the
// explicit confirmed destructive transaction.
bool commitAgentBehaviourRegistryDetach(
	std::shared_ptr<core::Building> const& building, std::string& diagnostic);
bool commitAgentBehaviourRegistryDetachClearingAssignments(
	std::shared_ptr<core::Building> const& building, std::string& diagnostic);
bool commitAgentBehaviourRegistrySwitch(
	std::shared_ptr<core::Building> const& building,
	std::string const& buildingFilepath, std::string const& packageDirectory,
	std::string& diagnostic);
bool commitAgentBehaviourRegistrySwitchClearingAssignments(
	std::shared_ptr<core::Building> const& building,
	std::string const& buildingFilepath, std::string const& packageDirectory,
	std::string& diagnostic);

void requestAgentBehaviourRegistryDetach(
	std::shared_ptr<core::Building> const& building);
void requestAgentBehaviourRegistrySwitch(
	std::shared_ptr<core::Building> const& building,
	std::string buildingFilepath, std::string packageDirectory);
bool agentBehaviourRegistryChangePending(std::string* consequence = nullptr);
bool confirmPendingAgentBehaviourRegistryChange(std::string& diagnostic);
void cancelPendingAgentBehaviourRegistryChange();

// Reload refuses dirty state and running dependents. Source/helper modules and
// every affected authored Agent configuration are preflighted in fresh
// budgeted runtimes before atomic adoption; no Agent callback executes. The
// panel wrapper logs and renders the aggregated diagnostics.
bool reloadAgentBehaviourRegistry(
	std::shared_ptr<core::AgentBehaviourRegistry> const& registry,
	std::string const& packageDirectory, std::string* diagnostic = nullptr);
bool saveAgentBehaviourRegistry(
	std::shared_ptr<core::AgentBehaviourRegistry> const& registry,
	std::string const& packageDirectory, std::string* diagnostic = nullptr);

bool attachedAgentBehaviourRegistryIsModified(
	std::shared_ptr<const core::Building> const& building);

// Definition and dependent-Building documents keep separate history entries.
DocumentHistory& agentBehaviourRegistryDocumentHistory(
	std::shared_ptr<core::AgentBehaviourRegistry> const& registry);
DocumentHistory& agentBehaviourBuildingDocumentHistory(
	std::shared_ptr<core::Building> const& building);
uint64_t loadedAgentBehaviourUsageCount(
	core::AgentBehaviourRegistry const& registry, core::AgentBehaviourId id);
std::string agentBehaviourDeleteConfirmationText(
	core::AgentBehaviourRegistry const& registry, core::AgentBehaviourId id);
bool commitAgentBehaviourDelete(
	std::shared_ptr<core::AgentBehaviourRegistry> const& registry,
	core::AgentBehaviourId id, std::string& diagnostic);
void requestAgentBehaviourDelete(
	std::shared_ptr<core::AgentBehaviourRegistry> const& registry,
	core::AgentBehaviourId id);
bool agentBehaviourDeletePending(core::AgentBehaviourId* id = nullptr,
	uint64_t* loadedAgentCount = nullptr,
	std::string* consequence = nullptr);
bool confirmPendingAgentBehaviourDelete(
	std::shared_ptr<core::AgentBehaviourRegistry> const& registry,
	std::string& diagnostic);
void cancelPendingAgentBehaviourDelete();
bool restoreAgentBehaviourRegistrySnapshot(
	std::shared_ptr<core::AgentBehaviourRegistry> const& registry, bool redo,
	std::string* diagnostic = nullptr);

void resetBehavioursPanelState();
void forgetAgentBehaviourRegistryDocument(
	std::shared_ptr<core::AgentBehaviourRegistry> const& registry);

using AgentBehaviourRegistryPathSelector
	= std::function<std::optional<std::string>()>;

// Renders attached-registry identity and behaviour definitions with
// diagnostics, plus create/select/detach/switch/reload actions. Returns true
// after the Building reference changed, allowing the caller to persist the
// changed Building immediately.
bool renderBehavioursPanel(std::shared_ptr<core::Building> const& building,
	std::string const& buildingFilepath,
	AgentBehaviourRegistryPathSelector const& selectPackageDirectory = {});
