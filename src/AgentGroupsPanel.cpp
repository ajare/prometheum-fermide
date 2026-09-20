// The Agents section's Agent-group table; see include/AgentGroupsPanel.h.
//
// The widgets here are deliberately thin over two commit functions. Every
// add and rename the user performs goes through captureDocumentSnapshot() ->
// Building operation -> commitDocumentEdit(), so one accepted operation is
// one undoable document edit and one refused operation is none. The headless
// smoke checks call those same functions, which is what pins that rule down.

#include "AgentGroupsPanel.h"

#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include "imgui/imgui.h"
#include "imgui/IconsFontAwesome5.h"

#include "core/AgentGroup.h"
#include "core/Building.h"
#include "core/Log.h"

#include "DocumentEdit.h"

using namespace std;

namespace
{
	// One byte past the longest legal name, for the NUL the widget's buffer
	// needs. A user is stopped by the naming rule they can see, never by a
	// buffer that ran out first.
	constexpr size_t NameBufferSize{ core::AgentGroup::MaxNameBytes + 1 };

	struct GroupNameEdit
	{
		std::array<char, NameBufferSize> text{};
		bool editing{ false };
		std::string previous;
		std::string diagnostic;
	};

	map<uint64_t, GroupNameEdit> gGroupNameEdits;
	bool gAddingGroup{ false };
	bool gFocusAddRow{ false };
	std::array<char, NameBufferSize> gNewGroupName{};
	string gAddDiagnostic;

	void loadIntoBuffer(std::array<char, NameBufferSize>& buffer, string const& value)
	{
		strncpy(buffer.data(), value.c_str(), buffer.size() - 1);
		buffer[buffer.size() - 1] = '\0';
	}

	// Inline rename for one group: committed on Enter or on focus loss, in the
	// style the Layer name editor established. A refused value leaves the group
	// untouched and reports why beside the field.
	void renderAgentGroupNameEditor(shared_ptr<core::Building> const& building,
		core::AgentGroupId id)
	{
		auto& edit = gGroupNameEdits[id.value];

		if (!edit.editing)
			loadIntoBuffer(edit.text, building->getAgentGroupName(id));

		ImGui::SetNextItemWidth(-1.0f);
		auto const submitted = ImGui::InputText("##agentGroupName", edit.text.data(),
			edit.text.size(), ImGuiInputTextFlags_EnterReturnsTrue);

		if (!edit.editing)
		{
			if (submitted || ImGui::IsItemActivated())
			{
				edit.editing = true;
				edit.previous = building->getAgentGroupName(id);
				edit.diagnostic.clear();
			}
			return;
		}

		if (!submitted && ImGui::IsItemFocused()) return;

		edit.editing = false;

		auto const next = string(edit.text.data());
		// Whitespace is not a rename: trimming to the name it already has is a
		// no-op, and a no-op must not put an entry on the undo stack.
		if (core::AgentGroup::trimName(next) == edit.previous)
		{
			edit.diagnostic.clear();
			return;
		}

		string diagnostic;
		if (!commitAgentGroupRename(building, id, next, diagnostic))
		{
			edit.diagnostic = diagnostic;
			core::addLogMessage("Agent groups", 0, core::LogLevel::Warning, diagnostic);
			return;
		}
		edit.diagnostic.clear();
	}

	// The blank row behind "Add Group". Nothing exists until a valid name is
	// submitted: closing the row without one creates no group and no history.
	void renderAgentGroupAddRow(shared_ptr<core::Building> const& building)
	{
		ImGui::TableNextRow();
		ImGui::PushID("addRow");
		ImGui::TableSetColumnIndex(0);

		if (gFocusAddRow)
		{
			ImGui::SetKeyboardFocusHere(0);
			gFocusAddRow = false;
		}

		ImGui::SetNextItemWidth(-1.0f);
		auto const submitted = ImGui::InputText("##newAgentGroupName", gNewGroupName.data(),
			gNewGroupName.size(), ImGuiInputTextFlags_EnterReturnsTrue);

		ImGui::SameLine();
		auto const confirmed = ImGui::Button(ICON_FA_CHECK);
		ImGui::SameLine();
		auto const cancelled = ImGui::Button(ICON_FA_TIMES);

		if (submitted || confirmed)
		{
			string diagnostic;
			auto const created = commitAgentGroupAdd(building, gNewGroupName.data(), diagnostic);
			if (created)
			{
				gAddDiagnostic.clear();
				loadIntoBuffer(gNewGroupName, "");
				// Stay in the row: groups are usually typed several at a time,
				// in the order they will be listed.
				ImGui::SetKeyboardFocusHere(-1);
			}
			else
			{
				gAddDiagnostic = diagnostic;
				core::addLogMessage("Agent groups", 0, core::LogLevel::Warning, diagnostic);
			}
		}

		// Abandoning the row is not a half-created group: nothing was committed,
		// so closing it leaves the Building and the undo stack untouched.
		if (cancelled)
		{
			gAddingGroup = false;
			gAddDiagnostic.clear();
			loadIntoBuffer(gNewGroupName, "");
		}

		if (!gAddDiagnostic.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", gAddDiagnostic.c_str());
		}

		ImGui::PopID();
	}
}

core::AgentGroupId commitAgentGroupAdd(shared_ptr<core::Building> const& building,
	string const& rawName, string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "There is no Building to add an Agent group to";
		return {};
	}

	// Snapshot first: the history entry has to hold the state from before the
	// group existed. If the Building refuses the name, the snapshot is dropped
	// uncommitted and the undo stack never sees it.
	auto const undo = captureDocumentSnapshot(building);
	try
	{
		auto const id = building->addAgentGroup(rawName);
		commitDocumentEdit(std::move(undo));
		return id;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return {};
	}
}

bool commitAgentGroupRename(shared_ptr<core::Building> const& building, core::AgentGroupId id,
	string const& rawName, string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "There is no Building to rename an Agent group in";
		return false;
	}

	auto const undo = captureDocumentSnapshot(building);
	if (!building->renameAgentGroup(id, rawName, &diagnostic)) return false;

	commitDocumentEdit(std::move(undo));
	return true;
}

void resetAgentGroupsPanelState()
{
	gGroupNameEdits.clear();
	gAddingGroup = false;
	gFocusAddRow = false;
	loadIntoBuffer(gNewGroupName, "");
	gAddDiagnostic.clear();
}

void renderAgentGroupsPanel(shared_ptr<core::Building> const& building)
{
	if (!building) return;

	ImGuiTableFlags const flags =
		ImGuiTableFlags_SizingStretchSame |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_BordersV |
		ImGuiTableFlags_ContextMenuInBody;

	if (ImGui::BeginTable("AgentGroups", 1, flags))
	{
		ImGui::TableSetupColumn("Name");
		ImGui::TableHeadersRow();

		// Creation order, straight off the Building's registry key order.
		for (auto const id : building->getAgentGroupIds())
		{
			ImGui::TableNextRow();
			ImGui::PushID(id.value);
			ImGui::TableSetColumnIndex(0);
			renderAgentGroupNameEditor(building, id);
			ImGui::PopID();
		}

		if (gAddingGroup) renderAgentGroupAddRow(building);

		ImGui::EndTable();
	}

	ImGui::Spacing();

	ImGui::BeginDisabled(gAddingGroup);
	if (ImGui::Button(ICON_FA_PLUS " Add Group"))
	{
		gAddingGroup = true;
		gFocusAddRow = true;
		gAddDiagnostic.clear();
		loadIntoBuffer(gNewGroupName, "");
	}
	ImGui::EndDisabled();
	if (gAddingGroup && ImGui::IsItemHovered())
		ImGui::SetTooltip("Finish the group being added first");

	ImGui::SameLine();
	auto const count = building->getAgentGroupCount();
	ImGui::TextDisabled("%u group%s", count, count == 1 ? "" : "s");
}
