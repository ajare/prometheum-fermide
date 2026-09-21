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

bool renderTagsPanel(shared_ptr<core::Building> const& building,
	string const& buildingFilepath)
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
	string diagnostic;
	auto const canCreate = canCreateAgentTagRegistry(building, buildingFilepath, &diagnostic);
	ImGui::BeginDisabled(!canCreate);
	auto const clicked = ImGui::Button("Create empty registry");
	ImGui::EndDisabled();
	if (!canCreate && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", diagnostic.c_str());
	if (!clicked) return false;

	auto undo = captureDocumentSnapshot(building);
	try
	{
		auto registry = core::createAndAttachAgentTagRegistry(*building, buildingFilepath);
		commitDocumentEdit(std::move(undo));
		core::addLogMessage("Tags", 0, core::LogLevel::Info,
			"Created Agent tag registry " + building->getAgentTagRegistryFilename()
			+ " (" + registry->getUuid() + ")");
		return true;
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Tags", 0, core::LogLevel::Error,
			"Could not create Agent tag registry: " + string(error.what()));
		return false;
	}
}
