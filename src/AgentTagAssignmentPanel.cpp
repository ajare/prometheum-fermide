#include "AgentTagAssignmentPanel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include "DocumentEdit.h"
#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/Building.h"
#include "core/Log.h"
#include "imgui/imgui.h"

using namespace std;

namespace
{
	constexpr size_t SearchBufferSize{ 64 };
	array<char, SearchBufferSize> gTagSearch{};
	core::Building const* gSearchBuilding{ nullptr };
	core::AgentId gSearchAgent{};

	bool matchesSearch(string const& name)
	{
		string needle(gTagSearch.data());
		if (needle.empty()) return true;
		string display = "#" + name;
		auto lower = [](unsigned char value) { return static_cast<char>(std::tolower(value)); };
		std::transform(needle.begin(), needle.end(), needle.begin(), lower);
		std::transform(display.begin(), display.end(), display.begin(), lower);
		return display.find(needle) != string::npos;
	}
}

bool commitAgentTagAssignment(shared_ptr<core::Building> const& building,
	core::AgentId agent, core::AgentTagId tag, bool assigned, string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "There is no Building in which to edit an Agent tag assignment";
		return false;
	}

	// Capture before asking the Building to mutate. A failed capture must not
	// produce an accepted but non-undoable edit.
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before editing its Agent tag assignment";
		return false;
	}

	bool const changed = assigned
		? building->assignAgentTag(agent, tag, &diagnostic)
		: building->removeAgentTag(agent, tag, &diagnostic);
	if (!changed) return false;

	commitDocumentEdit(std::move(undo));
	return true;
}

void resetAgentTagAssignmentPanelState()
{
	gTagSearch.fill('\0');
	gSearchBuilding = nullptr;
	gSearchAgent = {};
}

void renderAgentTagAssignmentChecklist(shared_ptr<core::Building> const& building,
	core::AgentId agent)
{
	ImGui::SeparatorText("Agent tags");
	if (!building) return;

	auto const agentLookup = building->lookupAgent(agent);
	if (!agentLookup)
	{
		ImGui::TextDisabled("The selected Agent is no longer available.");
		return;
	}
	if (!building->hasAttachedAgentTagRegistry())
	{
		ImGui::TextDisabled("No Agent tag registry attached.");
		return;
	}

	if (gSearchBuilding != building.get() || gSearchAgent != agent)
	{
		gSearchBuilding = building.get();
		gSearchAgent = agent;
		gTagSearch.fill('\0');
	}

	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint("##agentTagSearch", "Search tags...",
		gTagSearch.data(), gTagSearch.size());

	auto const& registry = building->getAgentTagRegistry();
	auto const ids = registry->getAgentTagIdsAlphabetically();
	bool anyVisible{ false };
	for (auto const tag : ids)
	{
		auto const& name = registry->getAgentTagName(tag);
		if (!matchesSearch(name)) continue;
		anyVisible = true;

		bool assigned = agentLookup.entity->hasAgentTag(tag);
		ImGui::PushID(tag.value);
		ImGui::BeginDisabled(!building->isSimulationPaused());
		bool const toggled = ImGui::Checkbox("##agentTagAssigned", &assigned);
		ImGui::EndDisabled();
		bool const hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
		ImGui::SameLine();
		ImGui::Text("#%s", name.c_str());
		if (hovered && !building->isSimulationPaused())
			ImGui::SetTooltip("Pause the simulation to edit Agent tag assignments");

		if (toggled)
		{
			string diagnostic;
			if (!commitAgentTagAssignment(building, agent, tag, assigned, diagnostic))
				core::addLogMessage("Agent tags", 0, core::LogLevel::Warning, diagnostic);
		}
		ImGui::PopID();
	}

	if (ids.empty()) ImGui::TextDisabled("The attached registry has no tags.");
	else if (!anyVisible) ImGui::TextDisabled("No tags match the search.");
}
