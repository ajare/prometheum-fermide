#include "AgentBehaviourAssignmentPanel.h"

#include <array>
#include <cstring>
#include <string>
#include <utility>

#include "DocumentEdit.h"
#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/Building.h"
#include "core/Log.h"
#include "core/Marker.h"
#include "imgui/imgui.h"

using namespace std;

namespace
{
	core::AgentBehaviourConfigurationValue initialValue(
		core::Building const& building, core::AgentBehaviourSchemaField const& field)
	{
		if (field.defaultValue) return *field.defaultValue;
		switch (field.type)
		{
		case core::AgentBehaviourSchemaType::Boolean: return false;
		case core::AgentBehaviourSchemaType::Integer: return int64_t{ 0 };
		case core::AgentBehaviourSchemaType::Number: return 0.0;
		case core::AgentBehaviourSchemaType::String: return string{};
		case core::AgentBehaviourSchemaType::Duration:
			return core::AgentBehaviourDuration{ 1 };
		case core::AgentBehaviourSchemaType::Marker:
		{
			auto const ids = building.getMarkerIds();
			return ids.empty() ? core::MarkerId{} : ids.front();
		}
		case core::AgentBehaviourSchemaType::List:
			return core::AgentBehaviourConfigurationList{};
		case core::AgentBehaviourSchemaType::Record:
		{
			core::AgentBehaviourConfigurationRecord record;
			for (auto const& child : field.children)
				record.emplace(child.name, initialValue(building, child));
			return record;
		}
		}
		return false;
	}

	core::AgentBehaviourConfiguration initialConfiguration(
		core::Building const& building, core::AgentBehaviour const& behaviour)
	{
		core::AgentBehaviourConfiguration result;
		for (auto const& field : behaviour.getSchema())
			result.emplace(field.name, initialValue(building, field));
		return result;
	}

	void logRefusal(string const& diagnostic)
	{
		if (!diagnostic.empty())
			core::addLogMessage("Agent behaviours", 0, core::LogLevel::Warning, diagnostic);
	}

	bool renderConfigurationValue(core::Building const& building,
		core::AgentBehaviourSchemaField const& field,
		core::AgentBehaviourConfigurationValue& value)
	{
		if (auto* boolValue = core::agentBehaviourConfigurationGetIf<bool>(&value))
		{
			bool edited = *boolValue;
			if (!ImGui::Checkbox("##value", &edited)) return false;
			*boolValue = edited;
			return true;
		}
		if (auto* integerValue = core::agentBehaviourConfigurationGetIf<int64_t>(&value))
		{
			int64_t edited = *integerValue;
			if (!ImGui::InputScalar("##value", ImGuiDataType_S64, &edited)) return false;
			*integerValue = edited;
			return true;
		}
		if (auto* numberValue = core::agentBehaviourConfigurationGetIf<double>(&value))
		{
			double edited = *numberValue;
			if (!ImGui::InputDouble("##value", &edited)) return false;
			*numberValue = edited;
			return true;
		}
		if (auto* stringValue = core::agentBehaviourConfigurationGetIf<string>(&value))
		{
			array<char, 512> buffer{};
			strncpy(buffer.data(), stringValue->c_str(), buffer.size() - 1);
			if (!ImGui::InputText("##value", buffer.data(), buffer.size(),
				ImGuiInputTextFlags_EnterReturnsTrue)) return false;
			*stringValue = buffer.data();
			return true;
		}
		if (auto* durationValue =
			core::agentBehaviourConfigurationGetIf<core::AgentBehaviourDuration>(&value))
		{
			uint64_t edited = durationValue->ticks;
			if (!ImGui::InputScalar("##ticks", ImGuiDataType_U64, &edited)) return false;
			durationValue->ticks = edited;
			return true;
		}
		if (auto* markerValue =
			core::agentBehaviourConfigurationGetIf<core::MarkerId>(&value))
		{
			auto marker = building.lookupMarker(*markerValue);
			auto const preview = marker ? marker->getName().c_str() : "Select Marker";
			bool changed = false;
			if (ImGui::BeginCombo("##marker", preview))
			{
				for (auto const id : building.getMarkerIds())
				{
					auto candidate = building.lookupMarker(id);
					if (candidate && ImGui::Selectable(candidate->getName().c_str(),
						id == *markerValue))
					{
						*markerValue = id;
						changed = true;
					}
				}
				ImGui::EndCombo();
			}
			return changed;
		}
		if (auto* list = core::agentBehaviourConfigurationGetIf<
			core::AgentBehaviourConfigurationList>(&value))
		{
			bool changed = false;
			ImGui::TextDisabled("%zu schedule entries", list->size());
			if (list->size() < core::MaxAgentBehaviourListElements
				&& ImGui::SmallButton("Add entry"))
			{
				list->push_back(initialValue(building, field.children.front()));
				changed = true;
			}
			for (size_t index = 0; index < list->size(); ++index)
			{
				ImGui::PushID(static_cast<int>(index));
				ImGui::SeparatorText(("Entry " + to_string(index + 1)).c_str());
				if (index != 0 && ImGui::SmallButton("Move up"))
				{
					swap((*list)[index], (*list)[index - 1]);
					changed = true;
				}
				if (index != 0) ImGui::SameLine();
				if (index + 1 < list->size() && ImGui::SmallButton("Move down"))
				{
					swap((*list)[index], (*list)[index + 1]);
					changed = true;
				}
				if (index + 1 < list->size()) ImGui::SameLine();
				if (ImGui::SmallButton("Remove"))
				{
					list->erase(list->begin() + static_cast<ptrdiff_t>(index));
					ImGui::PopID();
					return true;
				}
				changed = renderConfigurationValue(building, field.children.front(),
					(*list)[index]) || changed;
				ImGui::PopID();
			}
			return changed;
		}
		if (auto* record = core::agentBehaviourConfigurationGetIf<
			core::AgentBehaviourConfigurationRecord>(&value))
		{
			bool changed = false;
			for (auto const& child : field.children)
			{
				auto found = record->find(child.name);
				if (found == record->end()) continue;
				ImGui::PushID(child.name.c_str());
				ImGui::TextUnformatted(child.name.c_str());
				ImGui::SameLine();
				ImGui::SetNextItemWidth(-1.0f);
				changed = renderConfigurationValue(building, child, found->second) || changed;
				ImGui::PopID();
			}
			return changed;
		}
		ImGui::TextDisabled("Unsupported field type");
		return false;
	}
}

bool commitAgentBehaviourAssignment(shared_ptr<core::Building> const& building,
	core::AgentId agent, core::AgentBehaviourId behaviour, uint64_t revision,
	core::AgentBehaviourConfiguration const& configuration, string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "There is no Building in which to edit an Agent behaviour";
		return false;
	}
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before editing an Agent behaviour";
		return false;
	}
	if (!building->setAgentBehaviourAssignment(agent, behaviour, revision,
		configuration, &diagnostic)) return false;
	commitDocumentEdit(std::move(undo));
	return true;
}

bool commitAgentBehaviourClear(shared_ptr<core::Building> const& building,
	core::AgentId agent, string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "There is no Building in which to clear an Agent behaviour";
		return false;
	}
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before clearing an Agent behaviour";
		return false;
	}
	if (!building->clearAgentBehaviourAssignment(agent, &diagnostic)) return false;
	commitDocumentEdit(std::move(undo));
	return true;
}

void renderAgentBehaviourAssignmentCell(shared_ptr<core::Building> const& building,
	core::AgentId agent)
{
	if (!building || !building->hasAttachedAgentBehaviourRegistry())
	{
		ImGui::TextDisabled("None");
		return;
	}
	auto const& registry = building->getAgentBehaviourRegistry();
	auto const& assignment = building->getAgentBehaviourAssignment(agent);
	char const* preview = "None";
	if (assignment)
	{
		auto const* definition = registry->lookupAgentBehaviour(assignment->behaviour);
		preview = definition ? definition->getName().c_str() : "Invalid";
	}
	ImGui::BeginDisabled(!building->isSimulationPaused());
	if (ImGui::BeginCombo("##agentBehaviour", preview))
	{
		if (ImGui::Selectable("None", !assignment))
		{
			string diagnostic;
			if (assignment && !commitAgentBehaviourClear(building, agent, diagnostic))
				logRefusal(diagnostic);
		}
		for (auto const id : registry->getBehaviourIdsAlphabetically())
		{
			auto const* definition = registry->lookupAgentBehaviour(id);
			if (!definition) continue;
			bool const selected = assignment && assignment->behaviour == id;
			if (ImGui::Selectable(definition->getName().c_str(), selected))
			{
				string diagnostic;
				auto configuration = initialConfiguration(*building, *definition);
				if (!commitAgentBehaviourAssignment(building, agent, id,
					definition->getRevision(), configuration, diagnostic)) logRefusal(diagnostic);
			}
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
}

void renderAgentBehaviourConfigurationPanel(shared_ptr<core::Building> const& building,
	core::AgentId agent)
{
	ImGui::SeparatorText("Agent behaviour");
	if (!building) return;
	if (!building->hasAttachedAgentBehaviourRegistry())
	{
		ImGui::TextDisabled("No Agent behaviour registry attached.");
		return;
	}

	ImGui::SetNextItemWidth(-1.0f);
	renderAgentBehaviourAssignmentCell(building, agent);
	auto assignment = building->getAgentBehaviourAssignment(agent);
	if (!assignment) return;
	auto const& registry = building->getAgentBehaviourRegistry();
	auto const* behaviour = registry->lookupAgentBehaviour(assignment->behaviour);
	if (!behaviour)
	{
		ImGui::TextDisabled("The assigned behaviour is unavailable.");
		return;
	}
	ImGui::TextDisabled("Revision %llu", static_cast<unsigned long long>(assignment->revision));
	ImGui::BeginDisabled(!building->isSimulationPaused());
	auto edited = assignment->configuration;
	bool changed = false;
	for (auto const& field : behaviour->getSchema())
	{
		auto found = edited.find(field.name);
		if (found == edited.end()) continue;
		ImGui::PushID(field.name.c_str());
		ImGui::TextUnformatted(field.name.c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		changed = renderConfigurationValue(*building, field, found->second) || changed;
		ImGui::PopID();
	}
	if (changed)
	{
		string diagnostic;
		if (!commitAgentBehaviourAssignment(building, agent, assignment->behaviour,
			assignment->revision, edited, diagnostic)) logRefusal(diagnostic);
	}
	ImGui::EndDisabled();
}
