// The Agents table's Group cell; see include/AgentGroupAssignmentPanel.h.
//
// The widget is thin over one commit function, exactly as the Groups table is.
// Every assignment and clearing the user performs goes through
// captureDocumentSnapshot() -> Building operation -> commitDocumentEdit(),
// so one accepted assignment is one undoable document edit and one refused
// assignment is none. The headless smoke checks call that same function,
// which is what pins the rule down.

#include "AgentGroupAssignmentPanel.h"

#include <memory>
#include <string>

#include "imgui/imgui.h"

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
	if (ImGui::BeginCombo("##agentGroup", preview.c_str()))
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

		// `<none>` always leads the list, so clearing an assignment is never
		// buried under the groups it could be confused with.
		if (ImGui::Selectable(NoGroupLabel, !current)) apply({});

		// Creation order, straight off the Building's registry key order, so
		// the list a user picks from matches the list they typed.
		for (auto const id : building->getAgentGroupIds())
		{
			auto const groupLookup = building->lookupAgentGroup(id);
			if (!groupLookup) continue;

			// The ID is pushed rather than trusted to the label: a group named
			// "Crew##A" must not collide with a group named "Crew##B".
			ImGui::PushID(id.value);
			if (ImGui::Selectable(groupLookup.entity->getName().c_str(), id == current))
				apply(id);
			ImGui::PopID();
		}

		ImGui::EndCombo();
	}
}
