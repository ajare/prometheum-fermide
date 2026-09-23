#include "BehavioursPanel.h"

#include <filesystem>
#include <string>
#include <stdexcept>
#include <utility>

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
			ImGui::TextDisabled("The referenced Agent behaviour registry package is not loaded.");
			ImGui::TextWrapped(
				"The package '%s' beside this Building is missing, malformed, or has a different UUID. "
				"Repair or replace it, then reopen the Building.",
				building->getAgentBehaviourRegistryPackageName().c_str());
			return false;
		}

		ImGui::TextUnformatted("Agent behaviour registry package");
		ImGui::SameLine();
		ImGui::Text("%s", building->getAgentBehaviourRegistryPackageName().c_str());
		ImGui::TextDisabled("UUID %s", registry->getUuid().c_str());

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
				if (!diagnostic.empty())
					core::addLogMessage("Behaviours", 0, core::LogLevel::Warning, diagnostic);
			}
		}
		if (detachClicked)
		{
			string diagnostic;
			if (commitAgentBehaviourRegistryDetach(building, diagnostic)) return true;
			if (!diagnostic.empty())
				core::addLogMessage("Behaviours", 0, core::LogLevel::Warning, diagnostic);
		}

		string editDiagnostic;
		auto const definitionEditsAllowed
			= registry->definitionEditsAreAllowed(&editDiagnostic);
		if (registry->isModified())
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "Modified");
		}

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
			else ImGui::SetTooltip("Reload definitions and preflight Lua modules without running Agent callbacks");
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
		auto previousRegistry = building->hasAttachedAgentBehaviourRegistry()
			? building->getAgentBehaviourRegistry() : nullptr;
		auto const previousPackage = building->hasAgentBehaviourRegistryReference()
			? building->getAgentBehaviourRegistryPackageName() : string{};
		auto const previousUuid = building->hasAgentBehaviourRegistryReference()
			? building->getExpectedAgentBehaviourRegistryUuid() : string{};
		auto registry = core::selectAndAttachAgentBehaviourRegistry(
			*building, buildingFilepath, packageDirectory);
		if (building->getAgentBehaviourRegistryPackageName() == previousPackage
			&& building->getExpectedAgentBehaviourRegistryUuid() == previousUuid)
		{
			diagnostic = "The selected Agent behaviour registry is already attached";
			return false;
		}
		commitDocumentEdit(std::move(undo));
		if (previousRegistry != registry) releaseRegistryIfUnused(previousRegistry);
		resetBehavioursPanelState();
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			"Switched to Agent behaviour registry package "
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

bool reloadAgentBehaviourRegistry(
	shared_ptr<core::AgentBehaviourRegistry> const& registry,
	string const& packageDirectory, string* diagnostic)
{
	if (!core::reloadAgentBehaviourRegistryDocument(registry, packageDirectory,
		diagnostic))
		return false;
	resetBehavioursPanelState();
	core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
		"Reloaded Agent behaviour registry package " + packageDirectory);
	return true;
}

bool attachedAgentBehaviourRegistryIsModified(
	shared_ptr<const core::Building> const& building)
{
	return building && building->hasAttachedAgentBehaviourRegistry()
		&& building->getAgentBehaviourRegistry()->isModified();
}

void resetBehavioursPanelState()
{
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
		return renderAttachedRegistry(building, buildingFilepath, selectPackageDirectory);

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
