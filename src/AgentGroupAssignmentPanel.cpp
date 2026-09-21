// The Agents table's Group cell; see include/AgentGroupAssignmentPanel.h.
//
// The widget is thin over one commit function, exactly as the Groups table is.
// Every assignment and clearing the user performs goes through
// captureDocumentSnapshot() -> Building operation -> commitDocumentEdit(),
// so one accepted assignment is one undoable document edit and one refused
// assignment is none. The headless smoke checks call that same function,
// which is what pins the rule down.

#include "AgentGroupAssignmentPanel.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "core/Building.h"
#include "core/Log.h"

#include "DocumentEdit.h"

using namespace std;

namespace
{
	// The first choice in every Group cell, and what an Agent with no Agent
	// group reads back as. The label is the domain's own "no group" word, so
	// the cell never shows a blank where a choice should be.
	char const* const NoGroupLabel{ "<none>" };

	// ImGui reads "##" in a widget label as the start of an invisible ID
	// suffix and hides it and everything after it (ImGui::FindRenderedTextEnd,
	// reached from RenderTextClipped). A user-authored Agent group name is
	// display text and may legally hold that pair - "Crew##Night" is a valid
	// name - so a name never travels as a label here. The widget's label
	// carries an ID suffix alone, and the name is drawn through RenderText with
	// hide_text_after_hash cleared, which puts every byte of it on the screen
	// (and into ImGui's text log, which mirrors what was rendered).
	void renderLiteralText(ImVec2 pos, string const& text, ImVec2 clipMin,
		ImVec2 clipMax)
	{
		ImGui::PushClipRect(clipMin, clipMax, true);
		ImGui::RenderText(pos, text.c_str(), text.data() + text.size(), false);
		ImGui::PopClipRect();
	}

	// How wide the name is when nothing of it is hidden. Measuring the literal
	// text is what lets the list reserve room for a name ImGui would otherwise
	// have truncated to nothing.
	float literalTextWidth(string const& text)
	{
		return ImGui::CalcTextSize(text.c_str(), text.data() + text.size(), false).x;
	}

	// One row of the Group list. The Selectable is the control - it takes its
	// identity from the pushed AgentGroupId, never from the name - and the name
	// is painted over the row as literal text. Every row is laid out to the
	// same width so the list still fills the popup now that its labels carry
	// no text to size itself by.
	bool renderGroupChoice(char const* idLabel, string const& name, bool selected,
		float rowWidth)
	{
		auto const picked = ImGui::Selectable(idLabel, selected,
			ImGuiSelectableFlags_None, ImVec2(rowWidth, 0.0f));

		// Selectable() shrinks its bounding box by half the item spacing and
		// lays its text at the shrunken origin; adding that half back is where
		// the row's text begins.
		auto const& style = ImGui::GetStyle();
		ImVec2 const rowMin = ImGui::GetItemRectMin();
		ImVec2 const rowMax = ImGui::GetItemRectMax();
		renderLiteralText(
			ImVec2(rowMin.x + std::floor(style.ItemSpacing.x * 0.5f),
				rowMin.y + std::floor(style.ItemSpacing.y * 0.5f)),
			name, rowMin, rowMax);

		return picked;
	}
}

bool commitAgentGroupAssignment(shared_ptr<core::Building> const& building,
	core::AgentId agent, core::AgentGroupId group, string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "There is no Building to assign an Agent group in";
		return false;
	}

	// Snapshot first: the history entry has to hold the state from before the
	// assignment moved. If the Building refuses the Agent or the group, the
	// snapshot is dropped uncommitted and the undo stack never sees it.
	auto const undo = captureDocumentSnapshot(building);
	if (!building->setAgentGroup(agent, group, &diagnostic)) return false;

	commitDocumentEdit(std::move(undo));
	return true;
}

std::string agentGroupAssignmentLabel(core::Building const& building, core::AgentId agent)
{
	auto const lookup = building.lookupAgent(agent);
	if (!lookup) return std::string{ NoGroupLabel };

	auto const assigned = lookup.entity->getAgentGroupId();
	if (!assigned) return std::string{ NoGroupLabel };

	// The name is read from the Building on every call rather than cached with
	// the Agent, which is what makes a rename show up in every assigned row
	// immediately: the Agent holds the ID, the Building holds the name.
	auto const groupLookup = building.lookupAgentGroup(assigned);
	return groupLookup ? groupLookup.entity->getName() : std::string{ NoGroupLabel };
}

void renderAgentGroupAssignmentCell(shared_ptr<core::Building> const& building,
	core::AgentId agent)
{
	if (!building) return;

	auto const agentLookup = building->lookupAgent(agent);
	if (!agentLookup)
	{
		ImGui::TextDisabled("-");
		return;
	}

	auto const current = agentLookup.entity->getAgentGroupId();
	auto const preview = agentGroupAssignmentLabel(*building, agent);

	ImGui::SetNextItemWidth(-1.0f);
	auto const& style = ImGui::GetStyle();
	// The combo's frame, measured before it is opened so the preview can be
	// drawn into the cell's own window after the popup has had the frame.
	// A label-less combo lays its frame at the cursor, CalcItemWidth() wide and
	// GetFrameHeight() tall, which is what BeginCombo itself does here.
	ImVec2 const frameMin = ImGui::GetCursorScreenPos();
	ImVec2 const frameMax(frameMin.x + ImGui::CalcItemWidth(),
		frameMin.y + ImGui::GetFrameHeight());

	// CustomPreview keeps the selected name out of BeginCombo's preview path,
	// where a "##" in it would hide the rest. The name is drawn below instead.
	bool const listOpen = ImGui::BeginCombo("##agentGroup", "",
		ImGuiComboFlags_CustomPreview);
	if (listOpen)
	{
		auto const apply = [&](core::AgentGroupId group)
		{
			// Re-choosing what is already chosen is not an edit, and an edit
			// that changes nothing has no business on the undo stack.
			if (group == current) return;

			string diagnostic;
			if (!commitAgentGroupAssignment(building, agent, group, diagnostic))
				core::addLogMessage("Agent groups", 0, core::LogLevel::Warning, diagnostic);
		};

		// The popup sizes itself to its items, and its items no longer carry
		// the names that give it width, so the widest name is measured up front
		// and every row is laid out to it - or to the popup's own width, when
		// that is roomier, which is how the rows spanned the list before.
		float widest{ literalTextWidth(NoGroupLabel) };
		for (auto const id : building->getAgentGroupIds())
		{
			auto const groupLookup = building->lookupAgentGroup(id);
			if (!groupLookup) continue;
			widest = std::max(widest, literalTextWidth(groupLookup.entity->getName()));
		}
		float const rowWidth = std::max(ImGui::GetContentRegionAvail().x, widest);

		// `<none>` always leads the list, so clearing an assignment is never
		// buried under the groups it could be confused with.
		if (renderGroupChoice("##agentGroupNone", NoGroupLabel, !current, rowWidth))
			apply({});

		// Creation order, straight off the Building's registry key order, so
		// the list a user picks from matches the list they typed.
		for (auto const id : building->getAgentGroupIds())
		{
			auto const groupLookup = building->lookupAgentGroup(id);
			if (!groupLookup) continue;

			// The ID is pushed rather than trusted to the label: a group named
			// "Crew##A" must not collide with a group named "Crew##B", and
			// neither name reaches the widget that would read it as its own.
			ImGui::PushID(id.value);
			if (renderGroupChoice("##agentGroupChoice", groupLookup.entity->getName(),
				id == current, rowWidth))
				apply(id);
			ImGui::PopID();
		}

		ImGui::EndCombo();
	}

	// The preview fills the frame up to the arrow button, exactly where
	// BeginCombo would have drawn it, and shows the whole name.
	float const previewRight = std::max(frameMin.x,
		frameMax.x - ImGui::GetFrameHeight());
	renderLiteralText(
		ImVec2(frameMin.x + style.FramePadding.x, frameMin.y + style.FramePadding.y),
		preview, frameMin, ImVec2(previewRight, frameMax.y));
}
