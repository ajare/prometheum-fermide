#include "TagsPanel.h"

#include <filesystem>

#include "DocumentEdit.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Building.h"
#include "core/Log.h"
#include "imgui/imgui.h"

using namespace std;

bool canCreateAgentTagRegistry(shared_ptr<const core::Building> const& building,
	string const& buildingFilepath, string* diagnostic)
{
	auto refuse = [diagnostic](string message)
	{
		if (diagnostic) *diagnostic = std::move(message);
		return false;
	};
	if (!building) return refuse("No Building is open");
	if (building->hasAgentTagRegistryReference())
		return refuse("This Building already has an Agent tag registry");
	if (buildingFilepath.empty())
		return refuse("Save the Building before creating an Agent tag registry");

	error_code error;
	if (!filesystem::is_regular_file(buildingFilepath, error) || error)
		return refuse("Save the Building before creating an Agent tag registry");
	auto const registryPath = core::defaultAgentTagRegistryPath(buildingFilepath);
	if (filesystem::exists(registryPath, error) || error)
		return refuse("The adjacent registry file already exists");
	if (diagnostic) diagnostic->clear();
	return true;
}

bool canSelectAgentTagRegistry(shared_ptr<const core::Building> const& building,
	string const& buildingFilepath, string* diagnostic)
{
	auto refuse = [diagnostic](string message)
	{
		if (diagnostic) *diagnostic = std::move(message);
		return false;
	};
	if (!building) return refuse("No Building is open");
	if (building->hasAgentTagRegistryReference())
		return refuse("This Building already has an Agent tag registry");
	if (buildingFilepath.empty())
		return refuse("Save the Building before selecting an Agent tag registry");

	error_code error;
	if (!filesystem::is_regular_file(buildingFilepath, error) || error)
		return refuse("Save the Building before selecting an Agent tag registry");
	if (diagnostic) diagnostic->clear();
	return true;
}

bool renderTagsPanel(shared_ptr<core::Building> const& building,
	string const& buildingFilepath,
	AgentTagRegistryPathSelector const& selectRegistryPath)
{
	if (building->hasAgentTagRegistryReference())
	{
		ImGui::TextUnformatted("Agent tag registry");
		ImGui::SameLine();
		ImGui::Text("%s", building->getAgentTagRegistryFilename().c_str());
		ImGui::TextDisabled("UUID %s",
			building->getExpectedAgentTagRegistryUuid().c_str());
		return false;
	}

	ImGui::TextDisabled("No Agent tag registry attached.");
	string createDiagnostic;
	auto const canCreate = canCreateAgentTagRegistry(
		building, buildingFilepath, &createDiagnostic);
	ImGui::BeginDisabled(!canCreate);
	auto const createClicked = ImGui::Button("Create empty registry");
	ImGui::EndDisabled();
	if (!canCreate && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", createDiagnostic.c_str());

	ImGui::SameLine();
	string selectDiagnostic;
	auto canSelect = canSelectAgentTagRegistry(
		building, buildingFilepath, &selectDiagnostic);
	if (!selectRegistryPath)
	{
		canSelect = false;
		selectDiagnostic = "Registry file selection is unavailable";
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
			auto registry = core::createAndAttachAgentTagRegistry(*building, buildingFilepath);
			commitDocumentEdit(std::move(undo));
			core::addLogMessage("Tags", 0, core::LogLevel::Info,
				"Created Agent tag registry " + building->getAgentTagRegistryFilename()
				+ " (" + registry->getUuid() + ")");
			return true;
		}

		auto selectedPath = selectRegistryPath();
		if (!selectedPath) return false;
		auto undo = captureDocumentSnapshot(building);
		auto registry = core::selectAndAttachAgentTagRegistry(
			*building, buildingFilepath, *selectedPath);
		commitDocumentEdit(std::move(undo));
		core::addLogMessage("Tags", 0, core::LogLevel::Info,
			"Selected Agent tag registry " + building->getAgentTagRegistryFilename()
			+ " (" + registry->getUuid() + ")");
		return true;
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Tags", 0, core::LogLevel::Error,
			string(createClicked ? "Could not create" : "Could not select")
			+ " Agent tag registry: " + error.what());
		return false;
	}
}
