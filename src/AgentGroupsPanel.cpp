// The Agents section's Agent-group table; see include/AgentGroupsPanel.h.
//
// The widgets here are deliberately thin over a few commit functions. Every
// add, rename and delete the user performs goes through
// captureDocumentSnapshot() -> Building operation -> commitDocumentEdit(),
// so one accepted operation is one undoable document edit and one refused
// operation is none. The headless smoke checks call those same functions,
// which is what pins that rule down.

#include "AgentGroupsPanel.h"

#include <array>
#include <cstring>
#include <format>
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

	// The Groups table's columns, in declaration order. The panel below sets
	// up every column from this list, so the Active toggle, Agents count and
	// Delete columns are declared here once rather than being implied by bare
	// indexes somewhere.
	//
	// The name column takes the stretch: it holds an editor. Active and Delete
	// each hold one icon, while the count is a short read-only number, so those
	// columns stay fixed and never crowd the name out when the panel is narrow.
	std::vector<std::string> const kAgentGroupColumns{ "Name", "Active", "Agents", "Delete" };
	size_t const kActiveColumn{ 1 };
	size_t const kMemberCountColumn{ 2 };
	size_t const kDeleteColumn{ 3 };
	float const kIconColumnWidth{ 40.0f };

	// The confirmation's popup id. A plain string with no "##" decoration,
	// so the window ImGui builds for it carries this exact name, which is how
	// a headless check can assert the confirmation really reached the screen
	// instead of trusting a flag of our own.
	char const* const kDeletePopupId{ "Delete Agent group?" };

	// A deletion that has been asked for but not yet answered. Only an
	// occupied group ever gets here: an empty one is deleted on the spot, so
	// there is nothing to answer. Held by the panel rather than the Building,
	// because arming a dialog is editor state; the deletion itself is the
	// Building's, and stays behind commitAgentGroupDelete().
	struct PendingAgentGroupDelete
	{
		core::AgentGroupId id{};
		std::string text;
		uint32_t memberCount{ 0 };
		bool active{ false };
		bool openRequested{ false };
	};

	PendingAgentGroupDelete gPendingAgentGroupDelete;

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

bool commitAgentGroupDelete(shared_ptr<core::Building> const& building, core::AgentGroupId id,
	string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "There is no Building to delete an Agent group from";
		return false;
	}

	// One snapshot, one Building operation, one commit: the group and every
	// assignment cleared on its way out are inside the same document edit, so
	// no undo can ever land between them and leave Agents pointing at a group
	// that is back but missing, or a group gone with its Agents still on it.
	auto const undo = captureDocumentSnapshot(building);
	if (!building->deleteAgentGroup(id, &diagnostic)) return false;

	commitDocumentEdit(std::move(undo));
	return true;
}

bool agentGroupDeleteRequiresConfirmation(core::Building const& building,
	core::AgentGroupId id)
{
	// Straight through the Building, which counts the Agents that carry the
	// group's ID. The panel keeps no tally of its own, so the number that
	// decides whether to ask - and the number the confirmation then shows -
	// is one number, read from one place.
	return building.getAgentGroupMemberCount(id) > 0;
}

std::string agentGroupDeleteConfirmationText(core::Building const& building,
	core::AgentGroupId id)
{
	auto const count = building.getAgentGroupMemberCount(id);
	return std::format("Delete the Agent group \"{}\"? {} Agent{} assigned to it "
		"will return to no Agent group.",
		building.getAgentGroupName(id), count, count == 1 ? "" : "s");
}

void requestAgentGroupDelete(shared_ptr<core::Building> const& building,
	core::AgentGroupId id)
{
	if (!building) return;

	string diagnostic;
	if (!building->canDeleteAgentGroup(id, &diagnostic))
	{
		core::addLogMessage("Agent groups", 0, core::LogLevel::Warning, diagnostic);
		return;
	}

	// An empty group has nothing to warn about: deleting it is the whole of
	// the operation, so it happens now rather than behind a dialog that can
	// only ever be answered "yes".
	if (!agentGroupDeleteRequiresConfirmation(*building, id))
	{
		if (!commitAgentGroupDelete(building, id, diagnostic))
			core::addLogMessage("Agent groups", 0, core::LogLevel::Warning, diagnostic);
		return;
	}

	// Occupied: the impact is put to the user before anything is written.
	// The text is taken from the Building here, at the moment the request is
	// made, so the count the user reads is the authoritative one rather than
	// one the panel guessed at some earlier time.
	gPendingAgentGroupDelete.id = id;
	gPendingAgentGroupDelete.memberCount = building->getAgentGroupMemberCount(id);
	gPendingAgentGroupDelete.text = agentGroupDeleteConfirmationText(*building, id);
	gPendingAgentGroupDelete.active = true;
	gPendingAgentGroupDelete.openRequested = true;
}

bool agentGroupDeletePending(core::AgentGroupId* id, uint32_t* memberCount)
{
	if (id) *id = gPendingAgentGroupDelete.active ? gPendingAgentGroupDelete.id
		: core::AgentGroupId{};
	if (memberCount) *memberCount = gPendingAgentGroupDelete.active
		? gPendingAgentGroupDelete.memberCount : 0u;
	return gPendingAgentGroupDelete.active;
}

bool confirmPendingAgentGroupDelete(shared_ptr<core::Building> const& building,
	string& diagnostic)
{
	if (!gPendingAgentGroupDelete.active)
	{
		diagnostic = "No Agent group deletion is awaiting confirmation";
		return false;
	}

	// The request is spent the moment it is answered, whichever way the
	// deletion turns out: a delete that the Building refused should not stay
	// armed behind a popup the user has already closed.
	auto const id = gPendingAgentGroupDelete.id;
	cancelPendingAgentGroupDelete();

	return commitAgentGroupDelete(building, id, diagnostic);
}

void cancelPendingAgentGroupDelete()
{
	// Drops the request and nothing else. No snapshot was taken when the
	// request was armed, so there is nothing here to unwind: the Building,
	// its Agents, the dirty flag and the undo history were never touched.
	gPendingAgentGroupDelete = PendingAgentGroupDelete{};
}

void resetAgentGroupsPanelState()
{
	gGroupNameEdits.clear();
	gAddingGroup = false;
	gFocusAddRow = false;
	loadIntoBuffer(gNewGroupName, "");
	gAddDiagnostic.clear();
	// A document that was replaced, opened, or undone into place is not the
	// document a pending deletion was asked for, so a confirmation never
	// survives it: answering one would delete a group the user was never
	// shown the count of.
	cancelPendingAgentGroupDelete();
}

std::vector<std::string> const& agentGroupsPanelColumns()
{
	return kAgentGroupColumns;
}

void renderAgentGroupActivationCell(shared_ptr<core::Building> const& building,
	core::AgentGroupId id)
{
	if (!building) return;

	auto const memberCount = building->getAgentGroupMemberCount(id);
	auto const active = building->isAgentGroupActive(id);
	auto const editable = building->isSimulationPaused() && memberCount != 0;

	ImGui::BeginDisabled(!editable);
	ImGui::PushID("active");
	if (ImGui::Button(active ? ICON_FA_EYE : ICON_FA_EYE_SLASH,
		ImVec2(ImGui::GetFrameHeight(), 0.0f)))
	{
		string diagnostic;
		if (!building->setAgentGroupActive(id, !active, &diagnostic))
			core::addLogMessage("Agent groups", 0, core::LogLevel::Warning, diagnostic);
	}
	ImGui::PopID();
	ImGui::EndDisabled();

	if (ImGui::IsItemHovered())
	{
		if (!building->isSimulationPaused())
		{
			ImGui::SetTooltip("Pause the simulation to activate or deactivate this Agent group");
		}
		else if (memberCount == 0)
		{
			ImGui::SetTooltip("This Agent group has no Agents to activate or deactivate");
		}
		else
		{
			auto const tooltip = std::format("{} all {} Agent{} in the Agent group \"{}\"",
				active ? "Deactivate" : "Activate", memberCount, memberCount == 1 ? "" : "s",
				building->getAgentGroupName(id));
			ImGui::SetTooltip("%s", tooltip.c_str());
		}
	}
}

std::string agentGroupMemberCountLabel(core::Building const& building, core::AgentGroupId id)
{
	// Straight through the Building, which derives the count from the Agents
	// that carry the group's ID. No counter lives here, in the panel or on the
	// group, so there is nothing to keep in step with an assignment.
	return std::to_string(building.getAgentGroupMemberCount(id));
}

void renderAgentGroupMemberCountCell(core::Building const& building, core::AgentGroupId id)
{
	ImGui::TextUnformatted(agentGroupMemberCountLabel(building, id).c_str());
}

void renderAgentGroupDeleteCell(std::shared_ptr<core::Building> const& building,
	core::AgentGroupId id)
{
	if (!building) return;

	ImGui::PushID("delete");
	if (ImGui::Button(ICON_FA_TRASH, ImVec2(ImGui::GetFrameHeight(), 0.0f)))
		requestAgentGroupDelete(building, id);
	ImGui::PopID();

	// The tooltip says what the button would do before it is asked, in the
	// same terms the confirmation uses, so the two never disagree about how
	// many Agents a delete would hand back.
	if (ImGui::IsItemHovered())
	{
		auto const count = building->getAgentGroupMemberCount(id);
		auto const tooltip = count == 0
			? std::format("Delete the Agent group \"{}\"", building->getAgentGroupName(id))
			: std::format("Delete the Agent group \"{}\": {} Agent{} return to no Agent group",
				building->getAgentGroupName(id), count, count == 1 ? "" : "s");
		ImGui::SetTooltip("%s", tooltip.c_str());
	}
}

void renderAgentGroupDeleteConfirmation(std::shared_ptr<core::Building> const& building)
{
	// Asked for on this pass: open the modal now, in the same ID scope the
	// rest of this function uses, so the popup's identity is stable however
	// many frames it takes the user to answer.
	if (gPendingAgentGroupDelete.openRequested)
	{
		ImGui::OpenPopup(kDeletePopupId);
		gPendingAgentGroupDelete.openRequested = false;
	}

	// Dismissed without an answer - Escape, or a click outside the modal.
	// Neither is a delete, so the request is dropped rather than left armed
	// behind a popup that is no longer on screen to be answered.
	if (gPendingAgentGroupDelete.active && !ImGui::IsPopupOpen(kDeletePopupId))
	{
		cancelPendingAgentGroupDelete();
		return;
	}

	if (!ImGui::BeginPopupModal(kDeletePopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		return;

	// The popup outlived its request: the document was replaced, reopened or
	// undone while the confirmation was up, or the delete was answered from
	// somewhere other than the modal's own button. There is nothing left to
	// confirm, so the popup closes itself rather than lingering over the
	// editor with a question nobody asked.
	if (!gPendingAgentGroupDelete.active)
	{
		ImGui::TextUnformatted("There is no Agent group deletion to confirm.");
		ImGui::Separator();
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}

	ImGui::TextUnformatted(gPendingAgentGroupDelete.text.c_str());
	ImGui::Separator();

	if (ImGui::Button(ICON_FA_TRASH " Delete"))
	{
		string diagnostic;
		if (!confirmPendingAgentGroupDelete(building, diagnostic))
			core::addLogMessage("Agent groups", 0, core::LogLevel::Warning, diagnostic);
		ImGui::CloseCurrentPopup();
	}

	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_TIMES " Cancel"))
	{
		cancelPendingAgentGroupDelete();
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
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

	if (ImGui::BeginTable("AgentGroups",
		static_cast<int>(kAgentGroupColumns.size()), flags))
	{
		// The name column stretches, the count column keeps to its number, and
		// the Active and Delete columns hold one icon per group.
		ImGui::TableSetupColumn(kAgentGroupColumns[0].c_str(),
			ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(kAgentGroupColumns[kActiveColumn].c_str(),
			ImGuiTableColumnFlags_WidthFixed, kIconColumnWidth);
		ImGui::TableSetupColumn(kAgentGroupColumns[kMemberCountColumn].c_str(),
			ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(kAgentGroupColumns[kDeleteColumn].c_str(),
			ImGuiTableColumnFlags_WidthFixed, kIconColumnWidth);
		ImGui::TableHeadersRow();

		// Creation order, straight off the Building's registry key order.
		for (auto const id : building->getAgentGroupIds())
		{
			ImGui::TableNextRow();
			ImGui::PushID(id.value);
			ImGui::TableSetColumnIndex(0);
			renderAgentGroupNameEditor(building, id);

			// Bulk activation changes the members' own flags. In a mixed group the
			// eye remains open so one press can deactivate every active member.
			ImGui::TableSetColumnIndex(static_cast<int>(kActiveColumn));
			renderAgentGroupActivationCell(building, id);

			// The group's live membership count: every Agent in the Building
			// that carries this group's ID, wherever it is and whatever it is
			// doing. Recomputed each frame, which is what makes it live.
			ImGui::TableSetColumnIndex(static_cast<int>(kMemberCountColumn));
			renderAgentGroupMemberCountCell(*building, id);

			// Every group can be deleted, running or paused: grouping is
			// editor-only metadata, so nothing about a live simulation makes a
			// group safer to keep than one it is not running.
			ImGui::TableSetColumnIndex(static_cast<int>(kDeleteColumn));
			renderAgentGroupDeleteCell(building, id);
			ImGui::PopID();
		}

		if (gAddingGroup) renderAgentGroupAddRow(building);

		ImGui::EndTable();
	}

	// The confirmation lives with the panel that raises it, and is drawn on
	// every pass rather than only the one that armed it: a modal that stopped
	// being drawn would still block the editor, and one that was never drawn
	// again could never be answered.
	renderAgentGroupDeleteConfirmation(building);

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
