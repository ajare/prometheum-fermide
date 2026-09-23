#include "AgentBehaviourAssignmentPanel.h"

#include <array>
#include <cstring>
#include <limits>
#include <string>

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
	core::AgentBehaviourConfiguration initialConfiguration(
		core::Building const& building, core::AgentBehaviour const& behaviour)
	{
		core::AgentBehaviourConfiguration result;
		for (auto const& field : behaviour.getSchema())
		{
			if (field.defaultValue) { result.emplace(field.name, *field.defaultValue); continue; }
			switch (field.type)
			{
			case core::AgentBehaviourSchemaType::Boolean: result.emplace(field.name, false); break;
			case core::AgentBehaviourSchemaType::Integer: result.emplace(field.name, int64_t{ 0 }); break;
			case core::AgentBehaviourSchemaType::Number: result.emplace(field.name, 0.0); break;
			case core::AgentBehaviourSchemaType::String: result.emplace(field.name, string{}); break;
			case core::AgentBehaviourSchemaType::Duration:
				result.emplace(field.name, core::AgentBehaviourDuration{ 1 }); break;
			case core::AgentBehaviourSchemaType::Marker:
			{
				auto const ids = building.getMarkerIds();
				result.emplace(field.name, ids.empty() ? core::MarkerId{} : ids.front());
				break;
			}
			case core::AgentBehaviourSchemaType::List:
			case core::AgentBehaviourSchemaType::Record: break;
			}
		}
		return result;
	}

	void logRefusal(string const& diagnostic)
	{
		if (!diagnostic.empty())
			core::addLogMessage("Agent behaviours", 0, core::LogLevel::Warning, diagnostic);
	}

	bool commitValue(shared_ptr<core::Building> const& building, core::AgentId agent,
		core::AgentBehaviourAssignment const& current, string const& field,
		core::AgentBehaviourConfigurationValue value)
	{
		auto configuration = current.configuration;
		configuration[field] = std::move(value);
		string diagnostic;
		auto const changed = commitAgentBehaviourAssignment(building, agent,
			current.behaviour, current.revision, configuration, diagnostic);
		if (!changed) logRefusal(diagnostic);
		return changed;
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
	for (auto const& field : behaviour->getSchema())
	{
		auto const found = assignment->configuration.find(field.name);
		if (found == assignment->configuration.end()) continue;
		ImGui::PushID(field.name.c_str());
		ImGui::TextUnformatted(field.name.c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		auto const& value = found->second;
		if (auto const* boolValue = get_if<bool>(&value))
		{
			bool edited = *boolValue;
			if (ImGui::Checkbox("##value", &edited)) commitValue(building, agent, *assignment, field.name, edited);
		}
		else if (auto const* integerValue = get_if<int64_t>(&value))
		{
			int64_t edited = *integerValue;
			if (ImGui::InputScalar("##value", ImGuiDataType_S64, &edited))
				commitValue(building, agent, *assignment, field.name, edited);
		}
		else if (auto const* numberValue = get_if<double>(&value))
		{
			double edited = *numberValue;
			if (ImGui::InputDouble("##value", &edited))
				commitValue(building, agent, *assignment, field.name, edited);
		}
		else if (auto const* stringValue = get_if<string>(&value))
		{
			array<char, 512> buffer{};
			strncpy(buffer.data(), stringValue->c_str(), buffer.size() - 1);
			if (ImGui::InputText("##value", buffer.data(), buffer.size(),
				ImGuiInputTextFlags_EnterReturnsTrue))
				commitValue(building, agent, *assignment, field.name, string(buffer.data()));
		}
		else if (auto const* durationValue = get_if<core::AgentBehaviourDuration>(&value))
		{
			uint64_t edited = durationValue->ticks;
			if (ImGui::InputScalar("##value", ImGuiDataType_U64, &edited))
				commitValue(building, agent, *assignment, field.name,
					core::AgentBehaviourDuration{ edited });
		}
		else if (auto const* markerValue = get_if<core::MarkerId>(&value))
		{
			auto marker = building->lookupMarker(*markerValue);
			auto const preview = marker ? marker->getName().c_str() : "Select Marker";
			if (ImGui::BeginCombo("##value", preview))
			{
				for (auto const id : building->getMarkerIds())
				{
					auto candidate = building->lookupMarker(id);
					if (candidate && ImGui::Selectable(candidate->getName().c_str(), id == *markerValue))
						commitValue(building, agent, *assignment, field.name, id);
				}
				ImGui::EndCombo();
			}
		}
		else ImGui::TextDisabled("Unsupported field type");
		ImGui::PopID();
	}
	ImGui::EndDisabled();
}
