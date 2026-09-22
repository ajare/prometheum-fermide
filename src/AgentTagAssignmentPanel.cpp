#include "AgentTagAssignmentPanel.h"

#include <algorithm>
#include <array>
#include <string>

#include "DocumentEdit.h"
#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/Building.h"
#include "core/Log.h"
#include "imgui/imgui.h"

using namespace std;

namespace
{
	// The chip selection is transient per (Building, Agent) pair and clears when
	// the selection changes or the panel state is reset.
	core::AgentTagId gSelectedAssignedTag{};
	core::Building const* gChipBuilding{ nullptr };
	core::AgentId gChipAgent{};

	ImVec4 scaleColour(ImVec4 colour, float factor)
	{
		return ImVec4(std::min(colour.x * factor, 1.0f),
			std::min(colour.y * factor, 1.0f), std::min(colour.z * factor, 1.0f),
			colour.w);
	}

	// A single assigned tag drawn as a coloured chip showing just its name. The
	// chip uses the tag's intrinsic display Colour, with the text shaded for
	// contrast. Returns true when clicked.
	bool renderAssignedTagChip(core::AgentTagRegistry const& registry,
		core::AgentTagId tag, bool selected)
	{
		auto const& name = registry.getAgentTagName(tag);
		float rgb[3];
		core::agentColourToFloats(registry.getAgentTagDisplayColour(tag), rgb);
		ImVec4 const base(rgb[0], rgb[1], rgb[2], 1.0f);
		auto const luminance = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
		ImVec4 const text = luminance > 0.5f ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f)
			: ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

		ImGui::PushID(tag.value);
		ImGui::PushStyleColor(ImGuiCol_Button, base);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scaleColour(base, 1.25f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, scaleColour(base, 0.8f));
		ImGui::PushStyleColor(ImGuiCol_Text, text);
		if (selected)
		{
			ImGui::PushStyleColor(ImGuiCol_Border, text);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
		}
		auto const clicked = ImGui::SmallButton(("#" + name).c_str());
		if (selected)
		{
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
		}
		ImGui::PopStyleColor(4);
		ImGui::PopID();
		return clicked;
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
	gSelectedAssignedTag = {};
	gChipBuilding = nullptr;
	gChipAgent = {};
}

void renderAgentEffectiveProperties(shared_ptr<core::Building> const& building,
	core::AgentId agent)
{
	ImGui::SeparatorText("Effective Agent properties");
	if (!building) return;
	auto const lookup = building->lookupAgent(agent);
	if (!lookup)
	{
		ImGui::TextDisabled("The selected Agent is no longer available.");
		return;
	}

	auto const effective = lookup.entity->getEffectiveColour();
	if (effective.sourceTag && building->hasAttachedAgentTagRegistry())
	{
		auto const& registry = building->getAgentTagRegistry();
		ImGui::Text("Colour: RGB (%u, %u, %u) from #%s",
			static_cast<unsigned>(effective.value.r),
			static_cast<unsigned>(effective.value.g),
			static_cast<unsigned>(effective.value.b),
			registry->getAgentTagName(effective.sourceTag).c_str());
	}
	else
	{
		ImGui::Text("Colour: RGB (%u, %u, %u) (editor default)",
			static_cast<unsigned>(effective.value.r),
			static_cast<unsigned>(effective.value.g),
			static_cast<unsigned>(effective.value.b));
	}

	auto const walkSpeed = lookup.entity->getEffectiveWalkSpeedModifier();
	if (walkSpeed.sourceTag && building->hasAttachedAgentTagRegistry())
	{
		auto const& registry = building->getAgentTagRegistry();
		ImGui::Text("Walk speed modifier: %.3fx from #%s", walkSpeed.value,
			registry->getAgentTagName(walkSpeed.sourceTag).c_str());
	}
	else ImGui::Text("Walk speed modifier: 1.000x (base default)");

	auto const height = lookup.entity->getEffectiveHeightModifier();
	if (height.sourceTag && building->hasAttachedAgentTagRegistry())
	{
		auto const& registry = building->getAgentTagRegistry();
		ImGui::Text("Height modifier: %.3fx from #%s", static_cast<double>(height.value),
			registry->getAgentTagName(height.sourceTag).c_str());
	}
	else ImGui::Text("Height modifier: 1.000x (visual default)");
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

	if (gChipBuilding != building.get() || gChipAgent != agent)
	{
		gChipBuilding = building.get();
		gChipAgent = agent;
		gSelectedAssignedTag = {};
	}

	auto const& registry = building->getAgentTagRegistry();
	auto const ids = registry->getAgentTagIdsAlphabetically();
	auto const paused = building->isSimulationPaused();

	// Assigned tags only, drawn as a wrapping row of coloured chips.
	bool anyAssigned{ false };
	bool firstChip{ true };
	for (auto const tag : ids)
	{
		if (!agentLookup.entity->hasAgentTag(tag)) continue;
		anyAssigned = true;
		auto const chipWidth = ImGui::CalcTextSize(
			("#" + registry->getAgentTagName(tag)).c_str()).x
			+ 2.0f * ImGui::GetStyle().FramePadding.x;
		auto const rowEndX = ImGui::GetWindowPos().x
			+ ImGui::GetWindowContentRegionMax().x;
		if (!firstChip && ImGui::GetCursorPosX() + chipWidth < rowEndX)
			ImGui::SameLine();
		firstChip = false;
		if (renderAssignedTagChip(*registry, tag, gSelectedAssignedTag == tag))
			gSelectedAssignedTag = gSelectedAssignedTag == tag
				? core::AgentTagId{} : tag;
	}

	if (!anyAssigned) ImGui::TextDisabled("No tags assigned.");
	else
	{
		ImGui::TextDisabled("Select a tag and press Delete to remove it.");
		if (gSelectedAssignedTag
			&& ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
			&& ImGui::IsKeyPressed(ImGuiKey_Delete))
		{
			string diagnostic;
			if (!commitAgentTagAssignment(building, agent, gSelectedAssignedTag,
				false, diagnostic))
				core::addLogMessage("Agent tags", 0, core::LogLevel::Warning, diagnostic);
			gSelectedAssignedTag = {};
		}
	}

	// The combo lists every tag the Agent does not have yet. Conflicting tags
	// stay visible but disabled with the core validation diagnostic.
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::BeginDisabled(!paused);
	if (ImGui::BeginCombo("##addAgentTagToAgent", "Add tag..."))
	{
		bool anyAddable{ false };
		for (auto const tag : ids)
		{
			if (agentLookup.entity->hasAgentTag(tag)) continue;
			anyAddable = true;
			string assignmentDiagnostic;
			bool const compatible
				= building->canAssignAgentTag(agent, tag, &assignmentDiagnostic);
			ImGui::PushID(tag.value);
			ImGui::BeginDisabled(!compatible);
			bool const chosen = ImGui::Selectable(
				("#" + registry->getAgentTagName(tag)).c_str());
			ImGui::EndDisabled();
			if (!compatible
				&& ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", assignmentDiagnostic.c_str());
			ImGui::PopID();
			if (chosen)
			{
				string diagnostic;
				if (!commitAgentTagAssignment(building, agent, tag, true, diagnostic))
					core::addLogMessage("Agent tags", 0, core::LogLevel::Warning, diagnostic);
				ImGui::CloseCurrentPopup();
			}
		}
		if (!anyAddable) ImGui::TextDisabled("Every tag is already assigned.");
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	if (!paused
		&& ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Pause the simulation to edit Agent tag assignments");
}
