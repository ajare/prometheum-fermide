#include "BehavioursPanel.h"

#include <filesystem>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#include "DocumentEdit.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/Building.h"
#include "core/Log.h"
#include "imgui/IconsFontAwesome5.h"
#include "imgui/imgui.h"

using namespace std;

namespace
{
	vector<core::AgentBehaviourReloadDiagnostic> gReloadDiagnostics;

	struct PendingAgentBehaviourRegistryChange
	{
		weak_ptr<core::Building> building;
		string buildingFilepath;
		string packageDirectory;
		string consequence;
		bool detach{ false };
		bool active{ false };
		bool openRequested{ false };
	};
	PendingAgentBehaviourRegistryChange gPendingRegistryChange;

	string registryChangeConsequence(core::Building const& building, bool detach,
		string const& packageDirectory)
	{
		auto const assignments = building.getAgentBehaviourAssignmentCount();
		string result = "This destructive action will clear all "
			+ to_string(assignments) + " Agent behaviour assignment"
			+ (assignments == 1 ? "" : "s")
			+ " and configuration" + (assignments == 1 ? "" : "s") + ".\n";
		if (detach)
			result += "It will detach " + building.getAgentBehaviourRegistryPackageName() + ". ";
		else result += "It will replace the current reference with "
			+ filesystem::path(packageDirectory).filename().string() + ". ";
		result += "Registry package files will not be deleted, renamed, or rewritten.";
		return result;
	}

	filesystem::path attachedPackagePath(core::Building const& building,
		string const& buildingFilepath)
	{
		return filesystem::path(buildingFilepath).parent_path()
			/ building.getAgentBehaviourRegistryPackageName();
	}

	void releaseRegistryIfUnused(
		shared_ptr<core::AgentBehaviourRegistry> const& registry)
	{
		if (registry)
			(void)core::unloadAgentBehaviourRegistryDocumentIfUnused(registry);
	}

	void appendSchemaField(string& output,
		core::AgentBehaviourSchemaField const& field, string const& indent)
	{
		output += indent + field.name + ": ";
		switch (field.type)
		{
		case core::AgentBehaviourSchemaType::List:
			output += "list of ";
			if (!field.children.empty()) appendSchemaField(output, field.children.front(), "");
			else output += "?";
			output += "\n";
			break;
		case core::AgentBehaviourSchemaType::Record:
			output += "record\n";
			for (auto const& child : field.children)
				appendSchemaField(output, child, indent + "  ");
			break;
		default:
			output += core::agentBehaviourSchemaTypeName(field.type);
			output += "\n";
			break;
		}
	}

	bool renderAttachedRegistry(shared_ptr<core::Building> const& building,
		string const& buildingFilepath,
		AgentBehaviourRegistryPathSelector const& selectPackageDirectory)
	{
		auto const& registry = building->getAgentBehaviourRegistry();
		if (!registry)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
				"Agent behaviour dependency unavailable");
			ImGui::TextWrapped("%s",
				building->getAgentBehaviourDependencyDiagnostic().c_str());

			string selectDiagnostic;
			auto canSelect = canSelectAgentBehaviourRegistry(
				building, buildingFilepath, &selectDiagnostic);
			if (!selectPackageDirectory)
			{
				canSelect = false;
				selectDiagnostic = "Registry package selection is unavailable";
			}
			ImGui::BeginDisabled(!canSelect);
			auto const repairClicked = ImGui::Button("Repair or replace registry");
			ImGui::EndDisabled();
			if (!canSelect && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", selectDiagnostic.c_str());
			ImGui::SameLine();
			ImGui::BeginDisabled(!building->isSimulationPaused());
			auto const detachClicked = ImGui::Button("Detach registry");
			ImGui::EndDisabled();

			if (repairClicked)
			{
				try
				{
					auto selectedPath = selectPackageDirectory();
					if (selectedPath)
					{
						string diagnostic;
						if (commitAgentBehaviourRegistrySwitch(building,
							buildingFilepath, *selectedPath, diagnostic)) return true;
						if (building->getAgentBehaviourAssignmentCount() != 0
							&& diagnostic.find("confirmed destructive action") != string::npos)
							requestAgentBehaviourRegistrySwitch(building,
								buildingFilepath, *selectedPath);
						else if (!diagnostic.empty())
							core::addLogMessage("Behaviours", 0,
								core::LogLevel::Warning, diagnostic);
					}
				}
				catch (exception const& error)
				{
					core::addLogMessage("Behaviours", 0,
						core::LogLevel::Error, error.what());
				}
			}
			if (detachClicked)
			{
				if (building->getAgentBehaviourAssignmentCount() != 0)
					requestAgentBehaviourRegistryDetach(building);
				else
				{
					string diagnostic;
					if (commitAgentBehaviourRegistryDetach(building, diagnostic)) return true;
					if (!diagnostic.empty()) core::addLogMessage("Behaviours", 0,
						core::LogLevel::Warning, diagnostic);
				}
			}
			return false;
		}

		ImGui::TextUnformatted("Agent behaviour registry package");
		ImGui::SameLine();
		ImGui::Text("%s", building->getAgentBehaviourRegistryPackageName().c_str());
		ImGui::TextDisabled("UUID %s", registry->getUuid().c_str());
		ImGui::TextDisabled("Package revision %llu",
			static_cast<unsigned long long>(registry->getPackageRevision()));
		if (!building->agentBehaviourConfigurationsAreValid())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
				"Schema reconciliation required");
			ImGui::TextWrapped("%s",
				building->getAgentBehaviourDependencyDiagnostic().c_str());
		}

		string switchDiagnostic;
		auto canSwitch = canSelectAgentBehaviourRegistry(
			building, buildingFilepath, &switchDiagnostic);
		if (!selectPackageDirectory)
		{
			canSwitch = false;
			switchDiagnostic = "Registry package selection is unavailable";
		}
		ImGui::BeginDisabled(!canSwitch);
		auto const switchClicked = ImGui::Button("Switch registry");
		ImGui::EndDisabled();
		if (!canSwitch && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", switchDiagnostic.c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(!building->isSimulationPaused());
		auto const detachClicked = ImGui::Button("Detach registry");
		ImGui::EndDisabled();

		if (switchClicked)
		{
			optional<string> selectedPath;
			try { selectedPath = selectPackageDirectory(); }
			catch (exception const& error)
			{
				core::addLogMessage("Behaviours", 0, core::LogLevel::Error, error.what());
			}
			if (selectedPath)
			{
				string diagnostic;
				if (commitAgentBehaviourRegistrySwitch(building, buildingFilepath,
					*selectedPath, diagnostic)) return true;
				if (building->getAgentBehaviourAssignmentCount() != 0
					&& diagnostic.find("confirmed destructive action") != string::npos)
					requestAgentBehaviourRegistrySwitch(building,
						buildingFilepath, *selectedPath);
				else if (!diagnostic.empty())
					core::addLogMessage("Behaviours", 0, core::LogLevel::Warning, diagnostic);
			}
		}
		if (detachClicked)
		{
			if (building->getAgentBehaviourAssignmentCount() != 0)
				requestAgentBehaviourRegistryDetach(building);
			else
			{
				string diagnostic;
				if (commitAgentBehaviourRegistryDetach(building, diagnostic)) return true;
				if (!diagnostic.empty())
					core::addLogMessage("Behaviours", 0, core::LogLevel::Warning, diagnostic);
			}
		}

		string editDiagnostic;
		auto const definitionEditsAllowed
			= registry->definitionEditsAreAllowed(&editDiagnostic);
		if (registry->isModified())
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "Modified");
		}

		ImGui::BeginDisabled(!registry->isModified());
		if (ImGui::Button(ICON_FA_SAVE " Save registry"))
		{
			string diagnostic;
			if (!saveAgentBehaviourRegistry(registry,
				attachedPackagePath(*building, buildingFilepath).string(), &diagnostic))
				core::addLogMessage("Behaviours", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Persist reconciled schema history");
		ImGui::SameLine();
		ImGui::BeginDisabled(registry->isModified() || !definitionEditsAllowed);
		if (ImGui::Button(ICON_FA_SYNC " Reload registry"))
		{
			string diagnostic;
			if (!reloadAgentBehaviourRegistry(registry,
				attachedPackagePath(*building, buildingFilepath).string(), &diagnostic))
				core::addLogMessage("Behaviours", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			if (registry->isModified())
				ImGui::SetTooltip("Save or discard registry changes before reloading");
			else if (!definitionEditsAllowed)
				ImGui::SetTooltip("%s", editDiagnostic.c_str());
			else ImGui::SetTooltip("Reload source and helpers atomically after preflighting every affected Agent");
		}

		if (!gReloadDiagnostics.empty())
		{
			ImGui::SeparatorText("Reload diagnostics");
			ImGui::TextWrapped("The candidate package was not adopted (%u diagnostic%s).",
				static_cast<unsigned>(gReloadDiagnostics.size()),
				gReloadDiagnostics.size() == 1 ? "" : "s");
			for (auto const& item : gReloadDiagnostics)
			{
				if (item.scope == core::AgentBehaviourReloadDiagnosticScope::Agent)
				{
					if (!item.agentName.empty())
						ImGui::BulletText("Building %s / Agent %s (%llu) / %s",
							item.buildingName.c_str(), item.agentName.c_str(),
							static_cast<unsigned long long>(item.agent.value),
							item.moduleName.c_str());
					else ImGui::BulletText("Building %s", item.buildingName.c_str());
				}
				else if (item.scope == core::AgentBehaviourReloadDiagnosticScope::Module)
					ImGui::BulletText("Module %s", item.moduleName.c_str());
				else ImGui::BulletText("Package");
				ImGui::Indent();
				ImGui::TextWrapped("%s", item.diagnostic.c_str());
				if (!item.traceback.empty()
					&& item.traceback != item.diagnostic)
					ImGui::TextWrapped("%s", item.traceback.c_str());
				ImGui::Unindent();
			}
		}

		auto const helperNames = registry->getHelperModuleNames();
		if (!helperNames.empty())
		{
			ImGui::SeparatorText("Helper modules");
			for (auto const& name : helperNames)
			{
				auto const* helper = registry->lookupHelperModule(name);
				if (!helper) continue;
				auto const status = helper->getModuleStatus();
				auto const statusColour = status == core::AgentBehaviourModuleStatus::Loaded
					? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
					: status == core::AgentBehaviourModuleStatus::Error
						? ImVec4(1.0f, 0.35f, 0.3f, 1.0f)
						: ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
				ImGui::BulletText("%s (%s)", name.c_str(),
					helper->getSourceModulePath().c_str());
				ImGui::SameLine();
				ImGui::TextColored(statusColour, "%s",
					core::agentBehaviourModuleStatusName(status));
				if (!helper->getModuleDiagnostic().empty()
					&& ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", helper->getModuleDiagnostic().c_str());
			}
		}

		ImGui::SeparatorText("Behaviours");
		auto const ids = registry->getBehaviourIdsAlphabetically();
		ImGui::TextDisabled("%u behaviour%s", registry->getBehaviourCount(),
			registry->getBehaviourCount() == 1 ? "" : "s");
		if (ids.empty())
		{
			ImGui::TextWrapped(
				"This package declares no behaviours yet. Add behaviour definitions and Lua source "
				"modules to its behaviours.yaml manifest, then reload.");
		}
		for (auto const id : ids)
		{
			auto const* behaviour = registry->lookupAgentBehaviour(id);
			if (!behaviour) continue;
			ImGui::PushID(to_string(id.value).c_str());
			auto const open = ImGui::TreeNode("##definition");
			ImGui::SameLine();
			ImGui::TextUnformatted(behaviour->getName().c_str());
			ImGui::SameLine();
			auto const status = behaviour->getModuleStatus();
			auto const statusColour = status == core::AgentBehaviourModuleStatus::Loaded
				? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
				: status == core::AgentBehaviourModuleStatus::Error
					? ImVec4(1.0f, 0.35f, 0.3f, 1.0f)
					: ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
			ImGui::TextColored(statusColour, "%s",
				core::agentBehaviourModuleStatusName(status));
			if (open)
			{
				ImGui::BulletText("Revision %llu", (unsigned long long)behaviour->getRevision());
				ImGui::BulletText("Source module %s", behaviour->getSourceModulePath().c_str());
				if (!behaviour->getModuleDiagnostic().empty())
				{
					ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Preflight diagnostic:");
					ImGui::TextWrapped("%s", behaviour->getModuleDiagnostic().c_str());
				}
				if (!behaviour->getModuleTraceback().empty())
				{
					ImGui::TextUnformatted("Traceback:");
					ImGui::TextWrapped("%s", behaviour->getModuleTraceback().c_str());
				}
				if (behaviour->getSchema().empty())
					ImGui::BulletText("Configuration: none");
				else
				{
					ImGui::TextUnformatted("Configuration schema:");
					// Render one bullet per top-level field so nested records stay readable.
					for (auto const& field : behaviour->getSchema())
					{
						string fieldText;
						appendSchemaField(fieldText, field, "");
						ImGui::Bullet();
						ImGui::SameLine();
						ImGui::TextUnformatted(fieldText.c_str());
					}
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (!definitionEditsAllowed)
			ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s",
				editDiagnostic.c_str());
		ImGui::TextDisabled(
			"Definitions and Lua source are edited externally; Reload re-runs protected preflight.");
		return false;
	}

	bool renderRegistryChangeConfirmation()
	{
		if (gPendingRegistryChange.openRequested)
		{
			ImGui::OpenPopup("Clear Agent behaviour assignments?");
			gPendingRegistryChange.openRequested = false;
		}
		bool changed{ false };
		if (ImGui::BeginPopupModal("Clear Agent behaviour assignments?", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("%s", gPendingRegistryChange.consequence.c_str());
			if (ImGui::Button("Clear assignments and continue"))
			{
				string diagnostic;
				changed = confirmPendingAgentBehaviourRegistryChange(diagnostic);
				if (!changed && !diagnostic.empty())
					core::addLogMessage("Behaviours", 0,
						core::LogLevel::Error, diagnostic);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				cancelPendingAgentBehaviourRegistryChange();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		return changed;
	}
}

bool commitAgentBehaviourRegistryDetach(
	shared_ptr<core::Building> const& building, string& diagnostic)
{
	diagnostic.clear();
	if (!building || !building->hasAgentBehaviourRegistryReference())
	{
		diagnostic = "There is no Agent behaviour registry to detach";
		return false;
	}
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before detaching its Agent behaviour registry";
		return false;
	}
	try
	{
		auto const packageName = building->getAgentBehaviourRegistryPackageName();
		auto registry = building->getAgentBehaviourRegistry();
		building->detachAgentBehaviourRegistry();
		commitDocumentEdit(std::move(undo));
		releaseRegistryIfUnused(registry);
		resetBehavioursPanelState();
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			"Detached Agent behaviour registry package " + packageName
				+ " without changing its files");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

bool commitAgentBehaviourRegistryDetachClearingAssignments(
	shared_ptr<core::Building> const& building, string& diagnostic)
{
	diagnostic.clear();
	if (!building || !building->hasAgentBehaviourRegistryReference())
	{
		diagnostic = "There is no Agent behaviour registry to detach";
		return false;
	}
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before clearing Agent behaviour assignments and detaching its registry";
		return false;
	}
	try
	{
		auto const packageName = building->getAgentBehaviourRegistryPackageName();
		auto registry = building->getAgentBehaviourRegistry();
		building->detachAgentBehaviourRegistryAndClearAssignments();
		commitDocumentEdit(std::move(undo));
		releaseRegistryIfUnused(registry);
		resetBehavioursPanelState();
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			"Cleared all Agent behaviour assignments and configurations, then detached "
				+ packageName + " without changing its files");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

bool commitAgentBehaviourRegistrySwitch(
	shared_ptr<core::Building> const& building, string const& buildingFilepath,
	string const& packageDirectory, string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "No Building is open";
		return false;
	}
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before switching Agent behaviour registries";
		return false;
	}
	try
	{
		auto const previouslyAttached = building->hasAttachedAgentBehaviourRegistry();
		auto previousRegistry = previouslyAttached
			? building->getAgentBehaviourRegistry() : nullptr;
		auto const previousPackage = building->hasAgentBehaviourRegistryReference()
			? building->getAgentBehaviourRegistryPackageName() : string{};
		auto const previousUuid = building->hasAgentBehaviourRegistryReference()
			? building->getExpectedAgentBehaviourRegistryUuid() : string{};
		auto registry = core::selectAndAttachAgentBehaviourRegistry(
			*building, buildingFilepath, packageDirectory);
		auto const referenceChanged
			= building->getAgentBehaviourRegistryPackageName() != previousPackage
				|| building->getExpectedAgentBehaviourRegistryUuid() != previousUuid;
		if (!referenceChanged && previouslyAttached)
		{
			diagnostic = "The selected Agent behaviour registry is already attached";
			return false;
		}
		// Resolving the already-persisted expected package changes only runtime
		// dependency state, so it creates no authored undo entry or dirty state.
		if (referenceChanged) commitDocumentEdit(std::move(undo));
		if (previousRegistry != registry) releaseRegistryIfUnused(previousRegistry);
		resetBehavioursPanelState();
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			string(referenceChanged ? "Switched to" : "Recovered")
				+ " Agent behaviour registry package "
				+ building->getAgentBehaviourRegistryPackageName()
				+ " (" + registry->getUuid() + ")");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

bool commitAgentBehaviourRegistrySwitchClearingAssignments(
	shared_ptr<core::Building> const& building, string const& buildingFilepath,
	string const& packageDirectory, string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "No Building is open";
		return false;
	}
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before clearing Agent behaviour assignments and switching registries";
		return false;
	}
	try
	{
		auto previousRegistry = building->hasAttachedAgentBehaviourRegistry()
			? building->getAgentBehaviourRegistry() : nullptr;
		auto registry = core::selectAndAttachAgentBehaviourRegistryClearingAssignments(
			*building, buildingFilepath, packageDirectory);
		commitDocumentEdit(std::move(undo));
		if (previousRegistry != registry) releaseRegistryIfUnused(previousRegistry);
		resetBehavioursPanelState();
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			"Cleared all Agent behaviour assignments and configurations, then switched to "
				+ building->getAgentBehaviourRegistryPackageName()
				+ " (" + registry->getUuid() + ")");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

void requestAgentBehaviourRegistryDetach(
	shared_ptr<core::Building> const& building)
{
	if (!building || !building->hasAgentBehaviourRegistryReference()
		|| building->getAgentBehaviourAssignmentCount() == 0) return;
	gPendingRegistryChange.building = building;
	gPendingRegistryChange.consequence = registryChangeConsequence(
		*building, true, {});
	gPendingRegistryChange.detach = true;
	gPendingRegistryChange.active = true;
	gPendingRegistryChange.openRequested = true;
}

void requestAgentBehaviourRegistrySwitch(
	shared_ptr<core::Building> const& building, string buildingFilepath,
	string packageDirectory)
{
	if (!building || !building->hasAgentBehaviourRegistryReference()
		|| building->getAgentBehaviourAssignmentCount() == 0) return;
	gPendingRegistryChange.building = building;
	gPendingRegistryChange.buildingFilepath = std::move(buildingFilepath);
	gPendingRegistryChange.packageDirectory = std::move(packageDirectory);
	gPendingRegistryChange.consequence = registryChangeConsequence(*building,
		false, gPendingRegistryChange.packageDirectory);
	gPendingRegistryChange.detach = false;
	gPendingRegistryChange.active = true;
	gPendingRegistryChange.openRequested = true;
}

bool agentBehaviourRegistryChangePending(string* consequence)
{
	if (consequence) *consequence = gPendingRegistryChange.active
		? gPendingRegistryChange.consequence : string{};
	return gPendingRegistryChange.active;
}

bool confirmPendingAgentBehaviourRegistryChange(string& diagnostic)
{
	if (!gPendingRegistryChange.active)
	{
		diagnostic = "No Agent behaviour registry change is awaiting confirmation";
		return false;
	}
	auto pending = gPendingRegistryChange;
	cancelPendingAgentBehaviourRegistryChange();
	auto building = pending.building.lock();
	if (!building)
	{
		diagnostic = "The Building awaiting an Agent behaviour registry change is no longer open";
		return false;
	}
	if (pending.detach)
		return commitAgentBehaviourRegistryDetachClearingAssignments(
			building, diagnostic);
	return commitAgentBehaviourRegistrySwitchClearingAssignments(building,
		pending.buildingFilepath, pending.packageDirectory, diagnostic);
}

void cancelPendingAgentBehaviourRegistryChange()
{
	gPendingRegistryChange = PendingAgentBehaviourRegistryChange{};
}

bool reloadAgentBehaviourRegistry(
	shared_ptr<core::AgentBehaviourRegistry> const& registry,
	string const& packageDirectory, string* diagnostic)
{
	if (!core::reloadAgentBehaviourRegistryDocument(registry, packageDirectory,
		diagnostic, &gReloadDiagnostics))
		return false;
	resetBehavioursPanelState();
	core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
		"Reloaded Agent behaviour registry package " + packageDirectory);
	return true;
}

bool saveAgentBehaviourRegistry(
	shared_ptr<core::AgentBehaviourRegistry> const& registry,
	string const& packageDirectory, string* diagnostic)
{
	if (diagnostic) diagnostic->clear();
	if (!registry)
	{
		if (diagnostic) *diagnostic = "There is no Agent behaviour registry to save";
		return false;
	}
	try
	{
		registry->saveTo(core::agentBehaviourRegistryManifestPath(
			packageDirectory).string());
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			"Saved Agent behaviour registry package " + packageDirectory);
		return true;
	}
	catch (exception const& error)
	{
		if (diagnostic) *diagnostic = "Could not save Agent behaviour registry: "
			+ string(error.what());
		return false;
	}
}

bool attachedAgentBehaviourRegistryIsModified(
	shared_ptr<const core::Building> const& building)
{
	return building && building->hasAttachedAgentBehaviourRegistry()
		&& building->getAgentBehaviourRegistry()->isModified();
}

void resetBehavioursPanelState()
{
	gReloadDiagnostics.clear();
	cancelPendingAgentBehaviourRegistryChange();
}

void forgetAgentBehaviourRegistryDocument(
	shared_ptr<core::AgentBehaviourRegistry> const& registry)
{
	// Callers use this only after an explicit close/discard decision. Attached
	// registries remain manager-owned through their Building; an unreferenced
	// dirty registry may therefore be released here without pretending it saved.
	if (registry)
		(void)core::unloadAgentBehaviourRegistryDocumentIfUnused(registry, true);
	resetBehavioursPanelState();
}

bool canCreateAgentBehaviourRegistry(
	shared_ptr<const core::Building> const& building,
	string const& buildingFilepath, string* diagnostic)
{
	auto refuse = [diagnostic](string message)
	{
		if (diagnostic) *diagnostic = std::move(message);
		return false;
	};
	if (!building) return refuse("No Building is open");
	if (!building->isSimulationPaused()) return refuse("Pause the Building before creating a registry");
	if (building->hasAgentBehaviourRegistryReference())
		return refuse("This Building already has an Agent behaviour registry");
	if (buildingFilepath.empty())
		return refuse("Save the Building before creating an Agent behaviour registry");

	error_code error;
	if (!filesystem::is_regular_file(buildingFilepath, error) || error)
		return refuse("Save the Building before creating an Agent behaviour registry");
	auto const packagePath = core::defaultAgentBehaviourRegistryPackagePath(
		buildingFilepath);
	auto const status = filesystem::symlink_status(packagePath, error);
	if ((!error && status.type() != filesystem::file_type::not_found)
		|| (error && error != errc::no_such_file_or_directory))
		return refuse("The adjacent registry package already exists or cannot be inspected");
	if (diagnostic) diagnostic->clear();
	return true;
}

bool canSelectAgentBehaviourRegistry(
	shared_ptr<const core::Building> const& building,
	string const& buildingFilepath, string* diagnostic)
{
	auto refuse = [diagnostic](string message)
	{
		if (diagnostic) *diagnostic = std::move(message);
		return false;
	};
	if (!building) return refuse("No Building is open");
	if (!building->isSimulationPaused()) return refuse("Pause the Building before selecting a registry");
	if (buildingFilepath.empty())
		return refuse("Save the Building before selecting an Agent behaviour registry");

	error_code error;
	if (!filesystem::is_regular_file(buildingFilepath, error) || error)
		return refuse("Save the Building before selecting an Agent behaviour registry");
	if (diagnostic) diagnostic->clear();
	return true;
}

bool renderBehavioursPanel(shared_ptr<core::Building> const& building,
	string const& buildingFilepath,
	AgentBehaviourRegistryPathSelector const& selectPackageDirectory)
{
	if (building->hasAgentBehaviourRegistryReference())
	{
		auto const changed = renderAttachedRegistry(
			building, buildingFilepath, selectPackageDirectory);
		return renderRegistryChangeConfirmation() || changed;
	}

	ImGui::TextDisabled("No Agent behaviour registry attached.");
	string createDiagnostic;
	auto const canCreate = canCreateAgentBehaviourRegistry(
		building, buildingFilepath, &createDiagnostic);
	ImGui::BeginDisabled(!canCreate);
	auto const createClicked = ImGui::Button("Create empty registry");
	ImGui::EndDisabled();
	if (!canCreate && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", createDiagnostic.c_str());

	ImGui::SameLine();
	string selectDiagnostic;
	auto canSelect = canSelectAgentBehaviourRegistry(
		building, buildingFilepath, &selectDiagnostic);
	if (!selectPackageDirectory)
	{
		canSelect = false;
		selectDiagnostic = "Registry package selection is unavailable";
	}
	ImGui::BeginDisabled(!canSelect);
	auto const selectClicked = ImGui::Button("Select existing registry");
	ImGui::EndDisabled();
	if (!canSelect && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", selectDiagnostic.c_str());
	if (!createClicked && !selectClicked) return false;

	try
	{
		if (createClicked)
		{
			auto undo = captureDocumentSnapshot(building);
			if (!undo) throw runtime_error("Could not capture the Building before registry creation");
			auto registry = core::createAndAttachAgentBehaviourRegistry(
				*building, buildingFilepath);
			commitDocumentEdit(std::move(undo));
			core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
				"Created Agent behaviour registry package "
					+ building->getAgentBehaviourRegistryPackageName()
					+ " (" + registry->getUuid() + ")");
			return true;
		}

		auto selectedPath = selectPackageDirectory();
		if (!selectedPath) return false;
		string diagnostic;
		auto const changed = commitAgentBehaviourRegistrySwitch(building, buildingFilepath,
			*selectedPath, diagnostic);
		if (!changed && !diagnostic.empty())
			core::addLogMessage("Behaviours", 0, core::LogLevel::Error, diagnostic);
		return changed;
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Behaviours", 0, core::LogLevel::Error,
			string(createClicked ? "Could not create" : "Could not select")
				+ " Agent behaviour registry: " + error.what());
		return false;
	}
}
