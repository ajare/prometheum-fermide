#include "TagsPanel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

#include "DocumentEdit.h"
#include "BehavioursPanel.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/Building.h"
#include "core/Log.h"
#include "core/YamlSerializer.h"
#include "imgui/IconsFontAwesome5.h"
#include "imgui/imgui.h"

using namespace std;

namespace
{
	constexpr size_t NameBufferSize{ core::AgentTag::MaxNameCharacters + 1 };
	constexpr size_t SearchBufferSize{ 64 };
	char const* const DeletePopupId{ "Delete Agent tag?" };
	char const* const RegistryChangePopupId{ "Clear Agent tags and change registry?" };

	struct TagNameEdit
	{
		array<char, NameBufferSize> text{};
		bool editing{ false };
		string previous;
		string diagnostic;
	};

	struct TagDisplayColourEdit
	{
		array<float, 3> rgb{};
		core::AgentColour loaded{};
		bool initialised{ false };
		bool pending{ false };
		string diagnostic;
	};

	struct TagColourEdit
	{
		array<float, 3> rgb{};
		uint64_t loadedRevision{ 0 };
		bool pending{ false };
		string diagnostic;
	};

	struct TagWalkSpeedEdit
	{
		core::AgentModifierRange range{};
		uint64_t loadedRevision{ 0 };
		bool pending{ false };
		string diagnostic;
	};

	struct TagHeightEdit
	{
		core::AgentModifierRange range{};
		uint64_t loadedRevision{ 0 };
		bool pending{ false };
		string diagnostic;
	};

	struct RegistryBuildingSnapshot
	{
		core::Building* building{ nullptr };
		weak_ptr<void const> lifetime;
		string yaml;
		bool modified{ false };
		bool paused{ false };
	};

	struct RegistryEditSnapshotContext : DocumentSnapshotContext
	{
		weak_ptr<core::AgentTagRegistry> registry;
		core::AgentTagId affectedTag{};
		vector<RegistryBuildingSnapshot> buildings;

		bool isRestorable() const override
		{
			auto loadedRegistry = registry.lock();
			if (!loadedRegistry) return false;
			for (auto const& entry : buildings)
				if (entry.lifetime.expired()
					|| !loadedRegistry->hasLoadedBuilding(entry.building)) return false;
			return true;
		}
	};

	struct PendingAgentTagDelete
	{
		weak_ptr<core::AgentTagRegistry> registry;
		core::AgentTagId id{};
		string text;
		uint64_t loadedAgentCount{ 0 };
		bool active{ false };
		bool openRequested{ false };
	};

	struct PendingAgentTagRegistryChange
	{
		weak_ptr<core::Building> building;
		string buildingFilepath;
		string registryFilepath;
		string consequence;
		bool detach{ false };
		bool active{ false };
		bool openRequested{ false };
	};

	map<string, DocumentHistory> gRegistryHistories;
	map<uint64_t, TagNameEdit> gTagNameEdits;
	map<uint64_t, TagDisplayColourEdit> gTagDisplayColourEdits;
	map<uint64_t, TagColourEdit> gTagColourEdits;
	map<uint64_t, TagWalkSpeedEdit> gTagWalkSpeedEdits;
	map<uint64_t, TagHeightEdit> gTagHeightEdits;
	array<char, SearchBufferSize> gTagSearch{};
	PendingAgentTagDelete gPendingAgentTagDelete;
	PendingAgentTagRegistryChange gPendingAgentTagRegistryChange;
	bool gAddingTag{ false };
	bool gFocusAddTag{ false };
	array<char, NameBufferSize> gNewTagName{};
	string gAddTagDiagnostic;

	void loadIntoBuffer(array<char, NameBufferSize>& buffer, string const& value)
	{
		strncpy(buffer.data(), value.c_str(), buffer.size() - 1);
		buffer[buffer.size() - 1] = '\0';
	}

	string serializeBuilding(core::Building const& building)
	{
		auto serializer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		building.serialize(*serializer, workData);
		serializer->serialize();
		return serializer->getSerializedString();
	}

	optional<DocumentSnapshot> captureRegistrySnapshot(
		shared_ptr<core::AgentTagRegistry> const& registry,
		vector<core::Building*> const& participatingBuildings = {},
		core::AgentTagId affectedTag = {})
	{
		if (!registry) return nullopt;
		try
		{
			auto serializer = core::YamlSerializer::toString();
			core::SerializationWorkData workData;
			workData.markSerializedUnmodified = false;
			registry->serialize(*serializer, workData);
			serializer->serialize();
			auto snapshot = agentTagRegistryDocumentHistory(registry).capture(
				serializer->getSerializedString());

			if (affectedTag || !participatingBuildings.empty())
			{
				auto context = make_shared<RegistryEditSnapshotContext>();
				context->registry = registry;
				context->affectedTag = affectedTag;
				context->buildings.reserve(participatingBuildings.size());
				for (auto* building : participatingBuildings)
				{
					if (!registry->hasLoadedBuilding(building))
						throw runtime_error(
							"A Building participating in the tag edit is no longer loaded");
					context->buildings.push_back({ building, building->getLifetimeToken(),
						serializeBuilding(*building), building->isModified(),
						building->isSimulationPaused() });
				}
				snapshot.context = std::move(context);
			}
			return snapshot;
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Tags", 0, core::LogLevel::Error,
				"Could not capture Agent tag registry state: " + string(error.what()));
			return nullopt;
		}
	}

	filesystem::path attachedRegistryPath(core::Building const& building,
		string const& buildingFilepath)
	{
		if (buildingFilepath.empty() || !building.hasAgentTagRegistryReference()) return {};
		return filesystem::path(buildingFilepath).parent_path()
			/ building.getAgentTagRegistryFilename();
	}

	void releaseRegistryIfUnused(
		shared_ptr<core::AgentTagRegistry> const& registry, bool discardDirty = false)
	{
		if (!registry) return;
		auto const uuid = registry->getUuid();
		if (core::unloadAgentTagRegistryDocumentIfUnused(registry, discardDirty))
			gRegistryHistories.erase(uuid);
	}

	string registryChangeConsequence(core::Building const& building, bool detach,
		string const& registryFilepath)
	{
		auto const assignments = building.getAgentTagAssignmentCount();
		auto const agents = building.getAgentTagAssignedAgentCount();
		auto const samples = building.getAgentTagSampleCount();
		ostringstream text;
		text << (detach ? "Detach" : "Switch") << " Agent tag registry?\n"
			<< "This destructive action will:\n"
			<< "- remove all " << assignments << " Agent tag assignment"
			<< (assignments == 1 ? "" : "s") << " from " << agents << " Agent"
			<< (agents == 1 ? "" : "s") << "\n"
			<< "- clear all " << samples << " sampled Agent propert"
			<< (samples == 1 ? "y" : "ies") << "\n";
		if (detach)
		{
			text << "- detach " << building.getAgentTagRegistryFilename() << "\n"
				<< "The registry file will not be deleted or renamed.";
		}
		else
		{
			text << "- detach " << building.getAgentTagRegistryFilename()
				<< " and attach " << filesystem::path(registryFilepath).filename().string()
				<< " as the replacement registry\n"
				<< "Neither registry file will be deleted or renamed.\n"
				<< "Agent tag IDs will not be reinterpreted.";
		}
		return text.str();
	}

	void renderTagNameEditor(shared_ptr<core::AgentTagRegistry> const& registry,
		core::AgentTagId id)
	{
		auto& edit = gTagNameEdits[id.value];
		if (!edit.editing) loadIntoBuffer(edit.text, registry->getAgentTagName(id));

		ImGui::TextUnformatted("#");
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetNextItemWidth(
			-(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x));
		auto const submitted = ImGui::InputText("##agentTagName", edit.text.data(),
			edit.text.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		if (!edit.editing)
		{
			if (submitted || ImGui::IsItemActivated())
			{
				edit.editing = true;
				edit.previous = registry->getAgentTagName(id);
				edit.diagnostic.clear();
			}
			return;
		}
		if (!submitted && ImGui::IsItemFocused()) return;

		edit.editing = false;
		auto const next = string(edit.text.data());
		if (next == edit.previous)
		{
			edit.diagnostic.clear();
			return;
		}

		string diagnostic;
		if (!commitAgentTagRename(registry, id, next, diagnostic))
		{
			edit.diagnostic = diagnostic;
			core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
		}
		else edit.diagnostic.clear();
	}

	void renderTagAddRow(shared_ptr<core::AgentTagRegistry> const& registry)
	{
		ImGui::PushID("addRow");
		if (gFocusAddTag)
		{
			ImGui::SetKeyboardFocusHere(0);
			gFocusAddTag = false;
		}
		ImGui::TextUnformatted("#");
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetNextItemWidth(-(2.0f * ImGui::GetFrameHeight()
			+ 3.0f * ImGui::GetStyle().ItemSpacing.x));
		auto const submitted = ImGui::InputText("##newAgentTagName", gNewTagName.data(),
			gNewTagName.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		auto const confirmed = ImGui::Button(ICON_FA_CHECK "##addAgentTag");
		ImGui::SameLine();
		auto const cancelled = ImGui::Button(ICON_FA_TIMES "##cancelAgentTag");

		if (submitted || confirmed)
		{
			string diagnostic;
			auto const created = commitAgentTagAdd(registry, gNewTagName.data(), diagnostic);
			if (created)
			{
				gAddTagDiagnostic.clear();
				loadIntoBuffer(gNewTagName, "");
				ImGui::SetKeyboardFocusHere(-1);
			}
			else
			{
				gAddTagDiagnostic = diagnostic;
				core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
			}
		}
		if (cancelled)
		{
			gAddingTag = false;
			gAddTagDiagnostic.clear();
			loadIntoBuffer(gNewTagName, "");
		}
		if (!gAddTagDiagnostic.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
				gAddTagDiagnostic.c_str());
		ImGui::PopID();
	}

	// The intrinsic display Colour editor sits on the line directly after the
	// tag name. It only changes how the tag itself is rendered (its chip);
	// it never affects Agents, carries no revision, and cannot be removed.
	void renderTagDisplayColourEditor(
		shared_ptr<core::AgentTagRegistry> const& registry, core::AgentTagId id)
	{
		auto const value = registry->getAgentTagDisplayColour(id);
		auto& edit = gTagDisplayColourEdits[id.value];
		if (!edit.pending && (!edit.initialised || edit.loaded != value))
		{
			core::agentColourToFloats(value, edit.rgb.data());
			edit.loaded = value;
			edit.initialised = true;
			edit.diagnostic.clear();
		}

		ImGui::SetNextItemWidth(256.0f);
		if (ImGui::ColorEdit3("Tag Colour", edit.rgb.data(),
			ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_Uint8))
			edit.pending = true;
		auto const finished = ImGui::IsItemDeactivatedAfterEdit();
		auto const cancelled = ImGui::IsItemDeactivated() && !finished;
		if (edit.pending && finished)
		{
			string diagnostic;
			if (!commitAgentTagDisplayColourEdit(registry, id,
				core::agentColourFromFloats(edit.rgb.data()), diagnostic)
				&& diagnostic != "The tag display Colour is unchanged")
			{
				edit.diagnostic = diagnostic;
				core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
			}
			else edit.diagnostic.clear();
			edit.pending = false;
			edit.loaded = registry->getAgentTagDisplayColour(id);
		}
		else if (edit.pending && cancelled)
		{
			core::agentColourToFloats(edit.loaded, edit.rgb.data());
			edit.pending = false;
			edit.diagnostic.clear();
		}
		if (!edit.diagnostic.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
				edit.diagnostic.c_str());
	}

	void renderTagProperties(shared_ptr<core::AgentTagRegistry> const& registry,
		core::AgentTagId id)
	{
		auto const* colour = registry->getAgentTagColour(id);
		if (colour)
		{
			auto& edit = gTagColourEdits[id.value];
			if (!edit.pending && edit.loadedRevision != colour->revision)
			{
				core::agentColourToFloats(colour->value, edit.rgb.data());
				edit.loadedRevision = colour->revision;
				edit.diagnostic.clear();
			}

			ImGui::SetNextItemWidth(256.0f);
			if (ImGui::ColorEdit3("Colour", edit.rgb.data(),
				ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_Uint8))
				edit.pending = true;
			auto const finished = ImGui::IsItemDeactivatedAfterEdit();
			auto const cancelled = ImGui::IsItemDeactivated() && !finished;
			if (edit.pending && finished)
			{
				string diagnostic;
				if (!commitAgentTagColourEdit(registry, id,
					core::agentColourFromFloats(edit.rgb.data()), diagnostic)
					&& diagnostic != "The Agent Colour is unchanged")
				{
					edit.diagnostic = diagnostic;
					core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
				}
				else edit.diagnostic.clear();
				edit.pending = false;
				colour = registry->getAgentTagColour(id);
				if (colour) edit.loadedRevision = colour->revision;
			}
			else if (edit.pending && cancelled)
			{
				core::agentColourToFloats(colour->value, edit.rgb.data());
				edit.pending = false;
				edit.diagnostic.clear();
			}
			ImGui::SameLine();
			bool removed{ false };
			if (ImGui::Button(ICON_FA_TIMES "##removeColour"))
			{
				string diagnostic;
				if (!commitAgentTagColourRemove(registry, id, diagnostic))
				{
					edit.diagnostic = diagnostic;
					core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
				}
				else
				{
					gTagColourEdits.erase(id.value);
					colour = nullptr;
					removed = true;
				}
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove Colour");
			if (!removed && !edit.diagnostic.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
					edit.diagnostic.c_str());
		}

		auto const* walkSpeed = registry->getAgentTagWalkSpeedModifier(id);
		if (walkSpeed)
		{
			auto& edit = gTagWalkSpeedEdits[id.value];
			if (!edit.pending && edit.loadedRevision != walkSpeed->revision)
			{
				edit.range = walkSpeed->range;
				edit.loadedRevision = walkSpeed->revision;
				edit.diagnostic.clear();
			}
			ImGui::SetNextItemWidth(256.0f);
			if (ImGui::DragFloatRange2("Walk speed", &edit.range.minimum,
				&edit.range.maximum, 0.005f, core::AgentWalkSpeedModifierMinimum,
				core::AgentWalkSpeedModifierMaximum, "Min %.3f", "Max %.3f",
				ImGuiSliderFlags_AlwaysClamp))
				edit.pending = true;
			auto const finished = ImGui::IsItemDeactivatedAfterEdit();
			auto const cancelled = ImGui::IsItemDeactivated() && !finished;
			if (edit.pending && finished)
			{
				string diagnostic;
				if (!commitAgentTagWalkSpeedModifierEdit(
					registry, id, edit.range, diagnostic)
					&& diagnostic != "The Agent Walk speed modifier range is unchanged")
				{
					edit.diagnostic = diagnostic;
					core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
				}
				else edit.diagnostic.clear();
				edit.pending = false;
				walkSpeed = registry->getAgentTagWalkSpeedModifier(id);
				if (walkSpeed)
				{
					edit.range = walkSpeed->range;
					edit.loadedRevision = walkSpeed->revision;
				}
			}
			else if (edit.pending && cancelled)
			{
				edit.range = walkSpeed->range;
				edit.pending = false;
				edit.diagnostic.clear();
			}
			ImGui::SameLine();
			bool removed{ false };
			if (ImGui::Button(ICON_FA_TIMES "##removeWalkSpeed"))
			{
				string diagnostic;
				if (!commitAgentTagWalkSpeedModifierRemove(registry, id, diagnostic))
				{
					edit.diagnostic = diagnostic;
					core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
				}
				else
				{
					gTagWalkSpeedEdits.erase(id.value);
					walkSpeed = nullptr;
					removed = true;
				}
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove Walk speed modifier");
			if (!removed && !edit.diagnostic.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
					edit.diagnostic.c_str());
		}

		auto const* height = registry->getAgentTagHeightModifier(id);
		if (height)
		{
			auto& edit = gTagHeightEdits[id.value];
			if (!edit.pending && edit.loadedRevision != height->revision)
			{
				edit.range = height->range;
				edit.loadedRevision = height->revision;
				edit.diagnostic.clear();
			}
			ImGui::SetNextItemWidth(256.0f);
			if (ImGui::DragFloatRange2("Height", &edit.range.minimum,
				&edit.range.maximum, 0.005f, core::AgentHeightModifierMinimum,
				core::AgentHeightModifierMaximum, "Min %.3f", "Max %.3f",
				ImGuiSliderFlags_AlwaysClamp))
				edit.pending = true;
			auto const finished = ImGui::IsItemDeactivatedAfterEdit();
			auto const cancelled = ImGui::IsItemDeactivated() && !finished;
			if (edit.pending && finished)
			{
				string diagnostic;
				if (!commitAgentTagHeightModifierEdit(registry, id, edit.range, diagnostic)
					&& diagnostic != "The Agent Height modifier range is unchanged")
				{
					edit.diagnostic = diagnostic;
					core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
				}
				else edit.diagnostic.clear();
				edit.pending = false;
				height = registry->getAgentTagHeightModifier(id);
				if (height)
				{
					edit.range = height->range;
					edit.loadedRevision = height->revision;
				}
			}
			else if (edit.pending && cancelled)
			{
				edit.range = height->range;
				edit.pending = false;
				edit.diagnostic.clear();
			}
			ImGui::SameLine();
			bool removed{ false };
			if (ImGui::Button(ICON_FA_TIMES "##removeHeight"))
			{
				string diagnostic;
				if (!commitAgentTagHeightModifierRemove(registry, id, diagnostic))
				{
					edit.diagnostic = diagnostic;
					core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
				}
				else
				{
					gTagHeightEdits.erase(id.value);
					height = nullptr;
					removed = true;
				}
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove Height modifier");
			if (!removed && !edit.diagnostic.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
					edit.diagnostic.c_str());
		}

	}

	void renderTagAddPropertyDropdown(shared_ptr<core::AgentTagRegistry> const& registry,
		core::AgentTagId id)
	{
		auto const* colour = registry->getAgentTagColour(id);
		auto const* walkSpeed = registry->getAgentTagWalkSpeedModifier(id);
		auto const* height = registry->getAgentTagHeightModifier(id);
		auto const anyMissing = !colour || !walkSpeed || !height;
		ImGui::BeginDisabled(!anyMissing);
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo("##addAgentTagProperty", ICON_FA_PLUS " Add property"))
		{
			if (!colour && ImGui::Selectable("Colour"))
			{
				string diagnostic;
				if (!commitAgentTagColourAdd(registry, id, diagnostic))
					core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
				else gTagColourEdits.erase(id.value);
				ImGui::CloseCurrentPopup();
			}
			if (!walkSpeed && ImGui::Selectable("Walk speed modifier"))
			{
				string diagnostic;
				if (!commitAgentTagWalkSpeedModifierAdd(registry, id, diagnostic))
					core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
				else gTagWalkSpeedEdits.erase(id.value);
				ImGui::CloseCurrentPopup();
			}
			if (!height && ImGui::Selectable("Height modifier"))
			{
				string diagnostic;
				if (!commitAgentTagHeightModifierAdd(registry, id, diagnostic))
					core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
				else gTagHeightEdits.erase(id.value);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		if (!anyMissing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Every property is already added");
	}

	void renderTagDeleteButton(shared_ptr<core::AgentTagRegistry> const& registry,
		core::AgentTagId id)
	{
		if (ImGui::Button(ICON_FA_TRASH "##deleteAgentTag",
			ImVec2(ImGui::GetFrameHeight(), 0.0f)))
		{
			requestAgentTagDelete(registry, id);
		}
		if (ImGui::IsItemHovered())
		{
			auto const count = loadedAgentTagUsageCount(*registry, id);
			auto const tooltip = count == 0
				? format("Delete Agent tag #{}", registry->getAgentTagName(id))
				: format("Delete Agent tag #{} and remove {} loaded Agent assignment{}",
					registry->getAgentTagName(id), count, count == 1 ? "" : "s");
			ImGui::SetTooltip("%s", tooltip.c_str());
		}
	}

	void renderTagSection(shared_ptr<core::AgentTagRegistry> const& registry,
		core::AgentTagId id)
	{
		ImGui::PushID(id.value);
		ImGui::SeparatorText(format("#{}", registry->getAgentTagName(id)).c_str());
		renderTagNameEditor(registry, id);
		ImGui::SameLine();
		renderTagDeleteButton(registry, id);
		renderTagDisplayColourEditor(registry, id);
		auto const found = gTagNameEdits.find(id.value);
		if (found != gTagNameEdits.end() && !found->second.diagnostic.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
				found->second.diagnostic.c_str());
		renderTagProperties(registry, id);
		renderTagAddPropertyDropdown(registry, id);
		ImGui::TextDisabled("Loaded Agents: %llu", static_cast<unsigned long long>(
			loadedAgentTagUsageCount(*registry, id)));
		ImGui::PopID();
	}

	void renderTagDeleteConfirmation(
		shared_ptr<core::AgentTagRegistry> const& registry)
	{
		if (gPendingAgentTagDelete.openRequested)
		{
			ImGui::OpenPopup(DeletePopupId);
			gPendingAgentTagDelete.openRequested = false;
		}
		if (gPendingAgentTagDelete.active && !ImGui::IsPopupOpen(DeletePopupId))
		{
			cancelPendingAgentTagDelete();
			return;
		}
		if (!ImGui::BeginPopupModal(DeletePopupId, nullptr,
			ImGuiWindowFlags_AlwaysAutoResize)) return;

		if (!gPendingAgentTagDelete.active)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		ImGui::TextUnformatted(gPendingAgentTagDelete.text.c_str());
		ImGui::Separator();
		string editDiagnostic;
		auto const definitionEditsAllowed
			= registry->definitionEditsAreAllowed(&editDiagnostic);
		ImGui::BeginDisabled(!definitionEditsAllowed);
		if (ImGui::Button(ICON_FA_TRASH " Delete"))
		{
			string diagnostic;
			if (!confirmPendingAgentTagDelete(registry, diagnostic))
				core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		if (!definitionEditsAllowed && ImGui::IsItemHovered(
			ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", editDiagnostic.c_str());
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_TIMES " Cancel"))
		{
			cancelPendingAgentTagDelete();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	bool renderRegistryChangeConfirmation()
	{
		if (gPendingAgentTagRegistryChange.openRequested)
		{
			ImGui::OpenPopup(RegistryChangePopupId);
			gPendingAgentTagRegistryChange.openRequested = false;
		}
		if (gPendingAgentTagRegistryChange.active
			&& !ImGui::IsPopupOpen(RegistryChangePopupId))
		{
			cancelPendingAgentTagRegistryChange();
			return false;
		}
		if (!ImGui::BeginPopupModal(RegistryChangePopupId, nullptr,
			ImGuiWindowFlags_AlwaysAutoResize)) return false;

		if (!gPendingAgentTagRegistryChange.active)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return false;
		}
		ImGui::TextUnformatted(gPendingAgentTagRegistryChange.consequence.c_str());
		ImGui::Separator();
		bool changed{ false };
		if (ImGui::Button(ICON_FA_EXCLAMATION_TRIANGLE " Confirm destructive change"))
		{
			string diagnostic;
			changed = confirmPendingAgentTagRegistryChange(diagnostic);
			if (!changed && !diagnostic.empty())
				core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_TIMES " Cancel"))
		{
			cancelPendingAgentTagRegistryChange();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
		return changed;
	}

	bool renderAttachedRegistry(shared_ptr<core::Building> const& building,
		string const& buildingFilepath,
		AgentTagRegistryPathSelector const& selectRegistryPath)
	{
		auto const& registry = building->getAgentTagRegistry();
		if (!registry)
		{
			ImGui::TextDisabled("The referenced Agent tag registry is not loaded.");
			return false;
		}

		ImGui::TextUnformatted("Agent tag registry");
		ImGui::SameLine();
		ImGui::Text("%s", building->getAgentTagRegistryFilename().c_str());
		ImGui::TextDisabled("UUID %s", registry->getUuid().c_str());

		string switchDiagnostic;
		auto canSwitch = canSelectAgentTagRegistry(
			building, buildingFilepath, &switchDiagnostic);
		if (!selectRegistryPath)
		{
			canSwitch = false;
			switchDiagnostic = "Registry file selection is unavailable";
		}
		ImGui::BeginDisabled(!canSwitch);
		auto const switchClicked = ImGui::Button("Switch registry");
		ImGui::EndDisabled();
		if (!canSwitch && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", switchDiagnostic.c_str());
		ImGui::SameLine();
		auto const detachClicked = ImGui::Button("Detach registry");

		if (switchClicked)
		{
			auto selectedPath = selectRegistryPath();
			if (selectedPath)
			{
				if (building->getAgentTagAssignmentCount() != 0)
				{
					requestAgentTagRegistrySwitch(
						building, buildingFilepath, *selectedPath);
				}
				else
				{
					string diagnostic;
					if (commitAgentTagRegistrySwitch(building, buildingFilepath,
						*selectedPath, diagnostic)) return true;
					if (!diagnostic.empty())
						core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
				}
			}
		}
		if (detachClicked)
		{
			if (building->getAgentTagAssignmentCount() != 0)
				requestAgentTagRegistryDetach(building);
			else
			{
				string diagnostic;
				if (commitAgentTagRegistryDetach(building, diagnostic)) return true;
				if (!diagnostic.empty())
					core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
			}
		}

		auto& history = agentTagRegistryDocumentHistory(registry);
		string editDiagnostic;
		auto const definitionEditsAllowed
			= registry->definitionEditsAreAllowed(&editDiagnostic);
		if (agentTagRegistryIsModified(registry))
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "Modified");
		}

		ImGui::BeginDisabled(!agentTagRegistryIsModified(registry));
		if (ImGui::Button(ICON_FA_SAVE " Save registry"))
		{
			string diagnostic;
			if (!saveAgentTagRegistry(registry,
				attachedRegistryPath(*building, buildingFilepath).string(), &diagnostic))
				core::addLogMessage("Tags", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(agentTagRegistryIsModified(registry)
			|| !definitionEditsAllowed);
		if (ImGui::Button(ICON_FA_SYNC " Reload registry"))
		{
			string diagnostic;
			if (!reloadAgentTagRegistry(registry,
				attachedRegistryPath(*building, buildingFilepath).string(), &diagnostic))
				core::addLogMessage("Tags", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			if (agentTagRegistryIsModified(registry))
				ImGui::SetTooltip("Save or discard registry changes before reloading");
			else if (!definitionEditsAllowed)
				ImGui::SetTooltip("%s", editDiagnostic.c_str());
			else ImGui::SetTooltip("Reload external changes from disk");
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!history.canUndo() || !definitionEditsAllowed);
		if (ImGui::Button(ICON_FA_UNDO "##undoAgentTag"))
		{
			string diagnostic;
			if (!restoreAgentTagRegistrySnapshot(registry, false, &diagnostic)
				&& !diagnostic.empty())
				core::addLogMessage("Tags", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo registry edit");
		ImGui::SameLine();
		ImGui::BeginDisabled(!history.canRedo() || !definitionEditsAllowed);
		if (ImGui::Button(ICON_FA_REDO "##redoAgentTag"))
		{
			string diagnostic;
			if (!restoreAgentTagRegistrySnapshot(registry, true, &diagnostic)
				&& !diagnostic.empty())
				core::addLogMessage("Tags", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Redo registry edit");

		ImGui::SeparatorText("Tags");
		ImGui::BeginDisabled(!definitionEditsAllowed);
		ImGui::BeginDisabled(gAddingTag);
		if (ImGui::Button(ICON_FA_PLUS " Add Tag"))
		{
			gAddingTag = true;
			gFocusAddTag = true;
			gAddTagDiagnostic.clear();
			loadIntoBuffer(gNewTagName, "");
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		auto const count = registry->getAgentTagCount();
		ImGui::TextDisabled("%u tag%s", count, count == 1 ? "" : "s");
		if (gAddingTag) renderTagAddRow(registry);
		ImGui::EndDisabled();

		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##agentTagRegistrySearch", "Search tags...",
			gTagSearch.data(), gTagSearch.size());

		auto const ids = registry->getAgentTagIdsAlphabetically();
		bool anyVisible{ false };
		ImGui::BeginDisabled(!definitionEditsAllowed);
		for (auto const id : ids)
		{
			if (!agentTagNameMatchesFilter(registry->getAgentTagName(id),
				gTagSearch.data())) continue;
			anyVisible = true;
			renderTagSection(registry, id);
		}
		if (!ids.empty() && !anyVisible)
			ImGui::TextDisabled("No tags match the search.");
		ImGui::EndDisabled();
		if (!definitionEditsAllowed)
			ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s",
				editDiagnostic.c_str());
		ImGui::TextDisabled("Closed Buildings cannot be counted and may retain stale tag references after deletion.");
		renderTagDeleteConfirmation(registry);
		return renderRegistryChangeConfirmation();
	}
}

bool commitAgentTagRegistryDetach(shared_ptr<core::Building> const& building,
	string& diagnostic)
{
	diagnostic.clear();
	if (!building || !building->hasAgentTagRegistryReference())
	{
		diagnostic = "There is no Agent tag registry to detach";
		return false;
	}
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before detaching its Agent tag registry";
		return false;
	}
	try
	{
		auto const filename = building->getAgentTagRegistryFilename();
		auto registry = building->getAgentTagRegistry();
		building->detachAgentTagRegistry();
		commitDocumentEdit(std::move(undo));
		releaseRegistryIfUnused(registry);
		resetTagsPanelState();
		core::addLogMessage("Tags", 0, core::LogLevel::Info,
			"Detached Agent tag registry " + filename + " without changing its file");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

bool commitAgentTagRegistryDetachClearingAssignments(
	shared_ptr<core::Building> const& building, string& diagnostic)
{
	diagnostic.clear();
	if (!building || !building->hasAgentTagRegistryReference())
	{
		diagnostic = "There is no Agent tag registry to detach";
		return false;
	}
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before clearing Agent tags and detaching the registry";
		return false;
	}
	try
	{
		auto const filename = building->getAgentTagRegistryFilename();
		auto registry = building->getAgentTagRegistry();
		building->detachAgentTagRegistryAndClearAssignments();
		commitDocumentEdit(std::move(undo));
		releaseRegistryIfUnused(registry);
		resetTagsPanelState();
		core::addLogMessage("Tags", 0, core::LogLevel::Info,
			"Cleared all Agent tag assignments and samples, then detached "
			+ filename + " without changing its file");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

bool commitAgentTagRegistrySwitch(shared_ptr<core::Building> const& building,
	string const& buildingFilepath, string const& registryFilepath,
	string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "No Building is open";
		return false;
	}
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before switching Agent tag registries";
		return false;
	}
	try
	{
		auto previousRegistry = building->hasAttachedAgentTagRegistry()
			? building->getAgentTagRegistry() : nullptr;
		auto const previousFilename = building->hasAgentTagRegistryReference()
			? building->getAgentTagRegistryFilename() : string{};
		auto const previousUuid = building->hasAgentTagRegistryReference()
			? building->getExpectedAgentTagRegistryUuid() : string{};
		auto registry = core::selectAndAttachAgentTagRegistry(
			*building, buildingFilepath, registryFilepath);
		if (building->getAgentTagRegistryFilename() == previousFilename
			&& building->getExpectedAgentTagRegistryUuid() == previousUuid)
		{
			diagnostic = "The selected Agent tag registry is already attached";
			return false;
		}
		commitDocumentEdit(std::move(undo));
		(void)agentTagRegistryDocumentHistory(registry);
		if (previousRegistry != registry) releaseRegistryIfUnused(previousRegistry);
		resetTagsPanelState();
		core::addLogMessage("Tags", 0, core::LogLevel::Info,
			"Switched to Agent tag registry " + building->getAgentTagRegistryFilename()
				+ " (" + registry->getUuid() + ")");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

bool commitAgentTagRegistrySwitchClearingAssignments(
	shared_ptr<core::Building> const& building, string const& buildingFilepath,
	string const& registryFilepath, string& diagnostic)
{
	diagnostic.clear();
	if (!building)
	{
		diagnostic = "No Building is open";
		return false;
	}
	auto undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the Building before clearing Agent tags and switching registries";
		return false;
	}
	try
	{
		auto previousRegistry = building->hasAttachedAgentTagRegistry()
			? building->getAgentTagRegistry() : nullptr;
		auto const previousFilename = building->hasAgentTagRegistryReference()
			? building->getAgentTagRegistryFilename() : string{};
		auto const previousUuid = building->hasAgentTagRegistryReference()
			? building->getExpectedAgentTagRegistryUuid() : string{};
		auto registry = core::selectAndAttachAgentTagRegistryClearingAssignments(
			*building, buildingFilepath, registryFilepath);
		if (building->getAgentTagRegistryFilename() == previousFilename
			&& building->getExpectedAgentTagRegistryUuid() == previousUuid)
		{
			diagnostic = "The selected Agent tag registry is already attached";
			return false;
		}
		commitDocumentEdit(std::move(undo));
		(void)agentTagRegistryDocumentHistory(registry);
		if (previousRegistry != registry) releaseRegistryIfUnused(previousRegistry);
		resetTagsPanelState();
		core::addLogMessage("Tags", 0, core::LogLevel::Info,
			"Cleared all Agent tag assignments and samples, then switched to "
			+ building->getAgentTagRegistryFilename() + " (" + registry->getUuid() + ")");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

void requestAgentTagRegistryDetach(shared_ptr<core::Building> const& building)
{
	if (!building || !building->hasAgentTagRegistryReference()
		|| building->getAgentTagAssignmentCount() == 0) return;
	gPendingAgentTagRegistryChange.building = building;
	gPendingAgentTagRegistryChange.consequence
		= registryChangeConsequence(*building, true, {});
	gPendingAgentTagRegistryChange.detach = true;
	gPendingAgentTagRegistryChange.active = true;
	gPendingAgentTagRegistryChange.openRequested = true;
}

void requestAgentTagRegistrySwitch(shared_ptr<core::Building> const& building,
	string buildingFilepath, string registryFilepath)
{
	if (!building || !building->hasAgentTagRegistryReference()
		|| building->getAgentTagAssignmentCount() == 0) return;
	gPendingAgentTagRegistryChange.building = building;
	gPendingAgentTagRegistryChange.buildingFilepath = std::move(buildingFilepath);
	gPendingAgentTagRegistryChange.registryFilepath = std::move(registryFilepath);
	gPendingAgentTagRegistryChange.consequence = registryChangeConsequence(
		*building, false, gPendingAgentTagRegistryChange.registryFilepath);
	gPendingAgentTagRegistryChange.detach = false;
	gPendingAgentTagRegistryChange.active = true;
	gPendingAgentTagRegistryChange.openRequested = true;
}

bool agentTagRegistryChangePending(string* consequence)
{
	if (consequence) *consequence = gPendingAgentTagRegistryChange.active
		? gPendingAgentTagRegistryChange.consequence : string{};
	return gPendingAgentTagRegistryChange.active;
}

bool confirmPendingAgentTagRegistryChange(string& diagnostic)
{
	if (!gPendingAgentTagRegistryChange.active)
	{
		diagnostic = "No Agent tag registry change is awaiting confirmation";
		return false;
	}
	auto pending = gPendingAgentTagRegistryChange;
	cancelPendingAgentTagRegistryChange();
	auto building = pending.building.lock();
	if (!building)
	{
		diagnostic = "The Building awaiting an Agent tag registry change is no longer open";
		return false;
	}
	if (pending.detach)
		return commitAgentTagRegistryDetachClearingAssignments(building, diagnostic);
	return commitAgentTagRegistrySwitchClearingAssignments(building,
		pending.buildingFilepath, pending.registryFilepath, diagnostic);
}

void cancelPendingAgentTagRegistryChange()
{
	gPendingAgentTagRegistryChange = PendingAgentTagRegistryChange{};
}

bool agentTagNameMatchesFilter(string const& name, string const& filter)
{
	if (filter.empty()) return true;
	auto display = "#" + name;
	auto needle = filter;
	auto lower = [](unsigned char value) { return static_cast<char>(tolower(value)); };
	transform(display.begin(), display.end(), display.begin(), lower);
	transform(needle.begin(), needle.end(), needle.begin(), lower);
	return display.find(needle) != string::npos;
}

uint64_t loadedAgentTagUsageCount(core::AgentTagRegistry const& registry,
	core::AgentTagId id)
{
	return registry.getLoadedAgentTagUsageCount(id);
}

bool agentTagDeleteRequiresConfirmation(core::AgentTagRegistry const& registry,
	core::AgentTagId id)
{
	return loadedAgentTagUsageCount(registry, id) > 0;
}

string agentTagDeleteConfirmationText(core::AgentTagRegistry const& registry,
	core::AgentTagId id)
{
	auto const usage = registry.getLoadedAgentTagUsage(id);
	uint64_t total{ 0 };
	for (auto const& entry : usage) total += entry.agentCount;

	ostringstream text;
	text << "Delete Agent tag #" << registry.getAgentTagName(id) << "?\n"
		<< total << " loaded Agent" << (total == 1 ? " uses" : "s use")
		<< " this tag.";
	for (auto const& entry : usage)
	{
		if (!entry.building) continue;
		text << "\n- " << entry.building->getName() << ": " << entry.agentCount
			<< " Agent" << (entry.agentCount == 1 ? "" : "s");
	}
	text << "\nAll loaded assignments and samples sourced from this tag will be removed."
		<< "\nClosed Buildings cannot be counted. Any that retain this tag ID will be refused when loaded.";
	return text.str();
}

void requestAgentTagDelete(shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id)
{
	if (!registry) return;
	try
	{
		if (!agentTagDeleteRequiresConfirmation(*registry, id))
		{
			string diagnostic;
			if (!commitAgentTagDelete(registry, id, diagnostic))
				core::addLogMessage("Tags", 0, core::LogLevel::Warning, diagnostic);
			return;
		}
		gPendingAgentTagDelete.registry = registry;
		gPendingAgentTagDelete.id = id;
		gPendingAgentTagDelete.loadedAgentCount
			= loadedAgentTagUsageCount(*registry, id);
		gPendingAgentTagDelete.text = agentTagDeleteConfirmationText(*registry, id);
		gPendingAgentTagDelete.active = true;
		gPendingAgentTagDelete.openRequested = true;
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Tags", 0, core::LogLevel::Warning, error.what());
	}
}

bool agentTagDeletePending(core::AgentTagId* id, uint64_t* loadedAgentCount)
{
	if (id) *id = gPendingAgentTagDelete.active
		? gPendingAgentTagDelete.id : core::AgentTagId{};
	if (loadedAgentCount) *loadedAgentCount = gPendingAgentTagDelete.active
		? gPendingAgentTagDelete.loadedAgentCount : 0;
	return gPendingAgentTagDelete.active;
}

bool confirmPendingAgentTagDelete(
	shared_ptr<core::AgentTagRegistry> const& registry, string& diagnostic)
{
	if (!gPendingAgentTagDelete.active)
	{
		diagnostic = "No Agent tag deletion is awaiting confirmation";
		return false;
	}
	auto const expectedRegistry = gPendingAgentTagDelete.registry.lock();
	auto const id = gPendingAgentTagDelete.id;
	cancelPendingAgentTagDelete();
	if (!registry || registry != expectedRegistry)
	{
		diagnostic = "The pending Agent tag deletion belongs to another registry";
		return false;
	}
	return commitAgentTagDelete(registry, id, diagnostic);
}

void cancelPendingAgentTagDelete()
{
	gPendingAgentTagDelete = PendingAgentTagDelete{};
}

DocumentHistory& agentTagRegistryDocumentHistory(
	shared_ptr<core::AgentTagRegistry> const& registry)
{
	if (!registry) throw invalid_argument("There is no Agent tag registry document");
	auto const [entry, inserted] = gRegistryHistories.try_emplace(registry->getUuid());
	if (inserted && !registry->isModified()) entry->second.markSaved();
	return entry->second;
}

core::AgentTagId commitAgentTagAdd(shared_ptr<core::AgentTagRegistry> const& registry,
	string const& name, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry to add a tag to";
		return {};
	}
	auto undo = captureRegistrySnapshot(registry);
	if (!undo)
	{
		diagnostic = "Could not capture the Agent tag registry before adding a tag";
		return {};
	}
	try
	{
		auto const id = registry->addAgentTag(name);
		agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
		return id;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return {};
	}
}

bool commitAgentTagRename(shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, string const& name, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry in which to rename a tag";
		return false;
	}
	auto undo = captureRegistrySnapshot(registry);
	if (!undo)
	{
		diagnostic = "Could not capture the Agent tag registry before renaming a tag";
		return false;
	}
	if (!registry->renameAgentTag(id, name, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagDisplayColourEdit(
	shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, core::AgentColour colour, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry in which to edit a tag display Colour";
		return false;
	}
	auto undo = captureRegistrySnapshot(registry);
	if (!undo)
	{
		diagnostic = "Could not capture the Agent tag registry before editing a tag display Colour";
		return false;
	}
	if (!registry->setAgentTagDisplayColour(id, colour, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagColourAdd(shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry in which to add Colour";
		return false;
	}
	auto undo = captureRegistrySnapshot(registry);
	if (!undo)
	{
		diagnostic = "Could not capture the Agent tag registry before adding Colour";
		return false;
	}
	if (!registry->addAgentTagColour(id, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagColourEdit(shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, core::AgentColour colour, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry in which to edit Colour";
		return false;
	}
	auto undo = captureRegistrySnapshot(registry);
	if (!undo)
	{
		diagnostic = "Could not capture the Agent tag registry before editing Colour";
		return false;
	}
	if (!registry->setAgentTagColour(id, colour, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagColourRemove(shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry from which to remove Colour";
		return false;
	}
	auto undo = captureRegistrySnapshot(registry);
	if (!undo)
	{
		diagnostic = "Could not capture the Agent tag registry before removing Colour";
		return false;
	}
	if (!registry->removeAgentTagColour(id, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagWalkSpeedModifierAdd(
	shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry in which to add Walk speed modifier";
		return false;
	}
	vector<core::Building*> participants;
	try
	{
		for (auto const& usage : registry->getLoadedAgentTagUsage(id))
			if (usage.building && usage.agentCount > 0)
				participants.push_back(const_cast<core::Building*>(usage.building));
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
	auto undo = captureRegistrySnapshot(registry, participants, id);
	if (!undo)
	{
		diagnostic = "Could not capture the registry and loaded Buildings before adding Walk speed modifier";
		return false;
	}
	if (!registry->addAgentTagWalkSpeedModifier(id, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagWalkSpeedModifierEdit(
	shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, core::AgentModifierRange range, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry in which to edit Walk speed modifier";
		return false;
	}
	vector<core::Building*> participants;
	try
	{
		for (auto const& usage : registry->getLoadedAgentTagUsage(id))
			if (usage.building && usage.agentCount > 0)
				participants.push_back(const_cast<core::Building*>(usage.building));
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
	// The definition and all samples are one history entry. Capturing before the
	// core call also means validation failures and unchanged submissions commit
	// neither a partial Building snapshot nor an undo entry.
	auto undo = captureRegistrySnapshot(registry, participants, id);
	if (!undo)
	{
		diagnostic = "Could not capture the registry and loaded Buildings before editing Walk speed modifier";
		return false;
	}
	if (!registry->setAgentTagWalkSpeedModifier(id, range, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagWalkSpeedModifierRemove(
	shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry from which to remove Walk speed modifier";
		return false;
	}
	vector<core::Building*> participants;
	try
	{
		for (auto const& usage : registry->getLoadedAgentTagUsage(id))
			if (usage.building && usage.agentCount > 0)
				participants.push_back(const_cast<core::Building*>(usage.building));
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
	auto undo = captureRegistrySnapshot(registry, participants, id);
	if (!undo)
	{
		diagnostic = "Could not capture the registry and loaded Buildings before removing Walk speed modifier";
		return false;
	}
	if (!registry->removeAgentTagWalkSpeedModifier(id, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagHeightModifierAdd(
	shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry in which to add Height modifier";
		return false;
	}
	vector<core::Building*> participants;
	try
	{
		for (auto const& usage : registry->getLoadedAgentTagUsage(id))
			if (usage.building && usage.agentCount > 0)
				participants.push_back(const_cast<core::Building*>(usage.building));
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
	auto undo = captureRegistrySnapshot(registry, participants, id);
	if (!undo)
	{
		diagnostic = "Could not capture the registry and loaded Buildings before adding Height modifier";
		return false;
	}
	if (!registry->addAgentTagHeightModifier(id, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagHeightModifierEdit(
	shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, core::AgentModifierRange range, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry in which to edit Height modifier";
		return false;
	}
	vector<core::Building*> participants;
	try
	{
		for (auto const& usage : registry->getLoadedAgentTagUsage(id))
			if (usage.building && usage.agentCount > 0)
				participants.push_back(const_cast<core::Building*>(usage.building));
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
	auto undo = captureRegistrySnapshot(registry, participants, id);
	if (!undo)
	{
		diagnostic = "Could not capture the registry and loaded Buildings before editing Height modifier";
		return false;
	}
	if (!registry->setAgentTagHeightModifier(id, range, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagHeightModifierRemove(
	shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry from which to remove Height modifier";
		return false;
	}
	vector<core::Building*> participants;
	try
	{
		for (auto const& usage : registry->getLoadedAgentTagUsage(id))
			if (usage.building && usage.agentCount > 0)
				participants.push_back(const_cast<core::Building*>(usage.building));
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
	auto undo = captureRegistrySnapshot(registry, participants, id);
	if (!undo)
	{
		diagnostic = "Could not capture the registry and loaded Buildings before removing Height modifier";
		return false;
	}
	if (!registry->removeAgentTagHeightModifier(id, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool commitAgentTagDelete(shared_ptr<core::AgentTagRegistry> const& registry,
	core::AgentTagId id, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent tag registry from which to delete a tag";
		return false;
	}
	vector<core::Building*> participants;
	try
	{
		for (auto const& usage : registry->getLoadedAgentTagUsage(id))
			if (usage.building && usage.agentCount > 0)
				participants.push_back(const_cast<core::Building*>(usage.building));
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
	auto undo = captureRegistrySnapshot(registry, participants, id);
	if (!undo)
	{
		diagnostic = "Could not capture the registry and loaded Buildings before deleting the Agent tag";
		return false;
	}
	if (!registry->deleteAgentTag(id, &diagnostic)) return false;
	agentTagRegistryDocumentHistory(registry).commit(std::move(undo));
	return true;
}

bool restoreAgentTagRegistrySnapshot(shared_ptr<core::AgentTagRegistry> const& registry,
	bool redo, string* diagnostic)
{
	if (diagnostic) diagnostic->clear();
	if (!registry)
	{
		if (diagnostic) *diagnostic = "There is no Agent tag registry to restore";
		return false;
	}
	string pauseDiagnostic;
	if (!registry->definitionEditsAreAllowed(&pauseDiagnostic))
	{
		if (diagnostic) *diagnostic = pauseDiagnostic;
		return false;
	}
	auto& history = agentTagRegistryDocumentHistory(registry);
	auto const& source = redo ? history.redoEntries() : history.undoEntries();
	if (source.empty()) return false;
	auto targetContext = dynamic_pointer_cast<RegistryEditSnapshotContext>(
		source.back().context);
	vector<core::Building*> participants;
	if (targetContext)
	{
		participants.reserve(targetContext->buildings.size());
		for (auto const& entry : targetContext->buildings)
			participants.push_back(entry.building);
		// A tag restored by undo may have gained new loaded assignments before
		// redo. Include those Buildings in the inverse snapshot so redo clears
		// them and the following undo can restore them without stale references.
		if (targetContext->affectedTag
			&& registry->lookupAgentTag(targetContext->affectedTag))
		{
			for (auto const& usage : registry->getLoadedAgentTagUsage(
				targetContext->affectedTag))
			{
				auto* loaded = const_cast<core::Building*>(usage.building);
				if (loaded && usage.agentCount > 0
					&& find(participants.begin(), participants.end(), loaded)
						== participants.end())
					participants.push_back(loaded);
			}
		}
	}
	auto current = captureRegistrySnapshot(registry, participants,
		targetContext ? targetContext->affectedTag : core::AgentTagId{});
	if (!current) return false;

	try
	{
		bool propertyRevisionHighWaterAdvanced{ false };
		auto restore = [&registry, &propertyRevisionHighWaterAdvanced](
			DocumentSnapshot const& target)
		{
			// Parse and validate every document into temporary objects before the
			// shared live instance or any loaded Building is changed.
			auto replacement = core::AgentTagRegistry::create();
			auto registryReader = core::YamlSerializer::fromString(target.yaml);
			registryReader->deserialize();
			core::SerializationWorkData registryWork;
			if (!replacement->deserialize(*registryReader, registryWork)) return false;

			auto context = dynamic_pointer_cast<RegistryEditSnapshotContext>(
				target.context);
			vector<shared_ptr<core::Building>> validatedBuildings;
			if (context)
			{
				validatedBuildings.reserve(context->buildings.size());
				for (auto const& entry : context->buildings)
				{
					if (!registry->hasLoadedBuilding(entry.building))
						throw runtime_error(
							"A Building participating in this registry history entry is no longer loaded");
					auto candidate = make_shared<core::Building>("Loading", 1, 1);
					auto reader = core::YamlSerializer::fromString(entry.yaml);
					reader->deserialize();
					core::SerializationWorkData work;
					if (!candidate->deserialize(*reader, work)) return false;
					candidate->resolveAgentTagRegistry(replacement);
					validatedBuildings.push_back(std::move(candidate));
				}
			}

			// A redo may encounter assignments added since undo. Route the
			// deletion through the core cascade before installing the exact target
			// snapshots, so every currently loaded assignment is still removed.
			if (context && context->affectedTag
				&& registry->lookupAgentTag(context->affectedTag)
				&& !replacement->lookupAgentTag(context->affectedTag))
			{
				string deleteDiagnostic;
				if (!registry->deleteAgentTag(context->affectedTag, &deleteDiagnostic))
					throw runtime_error(deleteDiagnostic);
			}

			// Definition-only redo can become incompatible with assignments authored
			// after its undo. Judge the prospective registry against every currently
			// loaded Building before touching the shared live instance.
			string validationDiagnostic;
			vector<core::Building const*> restoredBuildings;
			if (context)
			{
				restoredBuildings.reserve(context->buildings.size());
				for (auto const& entry : context->buildings)
					restoredBuildings.push_back(entry.building);
			}
			if (!registry->loadedBuildingAssignmentsAreValid(
				*replacement, &validationDiagnostic, restoredBuildings))
				throw runtime_error(validationDiagnostic);

			// Validation succeeded as a whole. Restore the registry first, then
			// each dependent Building snapshot and reattach the same shared object.
			auto liveReader = core::YamlSerializer::fromString(target.yaml);
			liveReader->deserialize();
			core::SerializationWorkData liveRegistryWork;
			if (!registry->deserialize(*liveReader, liveRegistryWork)) return false;
			propertyRevisionHighWaterAdvanced = registry->getNextPropertyRevision()
				> replacement->getNextPropertyRevision();
			if (context)
			{
				for (auto const& entry : context->buildings)
				{
					auto reader = core::YamlSerializer::fromString(entry.yaml);
					reader->deserialize();
					core::SerializationWorkData work;
					if (!entry.building->deserialize(*reader, work)) return false;
					entry.building->resolveAgentTagRegistry(registry);
					if (entry.modified) entry.building->markModified();
					if (entry.paused) entry.building->pauseSimulation();
				}
			}
			return true;
		};
		auto const restored = redo
			? history.redo(std::move(current), restore)
			: history.undo(std::move(current), restore);
		if (!restored) return false;
		// Returning to a saved visible definition can still retain a newly issued
		// revision in the persisted allocator. That is a real registry change which
		// must be saved if non-reuse is to survive reopening the document.
		if (history.isModified() || propertyRevisionHighWaterAdvanced)
			registry->markModified();
		else registry->markUnmodified();
		resetTagsPanelState();
		return true;
	}
	catch (std::exception const& error)
	{
		if (diagnostic) *diagnostic = "Could not restore Agent tag registry: "
			+ string(error.what());
		return false;
	}
}

bool reloadAgentTagRegistry(shared_ptr<core::AgentTagRegistry> const& registry,
	string const& filepath, string* diagnostic)
{
	if (diagnostic) diagnostic->clear();
	if (!registry)
	{
		if (diagnostic) *diagnostic = "There is no Agent tag registry to reload";
		return false;
	}
	if (agentTagRegistryIsModified(registry))
	{
		if (diagnostic)
			*diagnostic = "The Agent tag registry has unsaved changes; save or discard them before reloading";
		return false;
	}

	string reloadDiagnostic;
	if (!core::reloadAgentTagRegistryDocument(
		registry, filepath, &reloadDiagnostic))
	{
		if (diagnostic) *diagnostic = std::move(reloadDiagnostic);
		return false;
	}

	// Every old entry describes definitions that are no longer live. A reload is
	// the new saved baseline, not an undoable editor command.
	auto& history = agentTagRegistryDocumentHistory(registry);
	history.clear();
	history.markSaved();
	resetTagsPanelState();
	core::addLogMessage("Tags", 0, core::LogLevel::Info,
		"Reloaded Agent tag registry from " + filepath);
	return true;
}

bool saveAgentTagRegistry(shared_ptr<core::AgentTagRegistry> const& registry,
	string const& filepath, string* diagnostic)
{
	if (diagnostic) diagnostic->clear();
	if (!registry)
	{
		if (diagnostic) *diagnostic = "There is no Agent tag registry to save";
		return false;
	}
	if (filepath.empty())
	{
		if (diagnostic) *diagnostic = "The Agent tag registry has no file path";
		return false;
	}
	try
	{
		registry->saveTo(filepath);
		agentTagRegistryDocumentHistory(registry).markSaved();
		releaseRegistryIfUnused(registry);
		core::addLogMessage("Tags", 0, core::LogLevel::Info,
			"Saved Agent tag registry to " + filepath);
		return true;
	}
	catch (std::exception const& error)
	{
		if (diagnostic) *diagnostic = "Could not save Agent tag registry: "
			+ string(error.what());
		return false;
	}
}

bool agentTagRegistryIsModified(shared_ptr<core::AgentTagRegistry> const& registry)
{
	return registry && (registry->isModified()
		|| agentTagRegistryDocumentHistory(registry).isModified());
}

bool attachedAgentTagRegistryIsModified(shared_ptr<const core::Building> const& building)
{
	if (!building || !building->hasAttachedAgentTagRegistry()) return false;
	return agentTagRegistryIsModified(building->getAgentTagRegistry());
}

namespace
{
	bool buildingDocumentIsModified(BuildingDocumentSaveTarget const& target)
	{
		return target.building && (target.building->isModified()
			|| (target.buildingHistory && target.buildingHistory->isModified()));
	}

	filesystem::path registrySavePath(BuildingDocumentSaveTarget const& target)
	{
		if (!target.registryFilepath.empty()) return target.registryFilepath;
		if (!target.building || target.buildingFilepath.empty()
			|| !target.building->hasAgentTagRegistryReference()) return {};
		return filesystem::path(target.buildingFilepath).parent_path()
			/ target.building->getAgentTagRegistryFilename();
	}

	filesystem::path behaviourPackageSavePath(
		BuildingDocumentSaveTarget const& target)
	{
		if (!target.behaviourPackagePath.empty()) return target.behaviourPackagePath;
		if (!target.building || target.buildingFilepath.empty()
			|| !target.building->hasAgentBehaviourRegistryReference()) return {};
		return filesystem::path(target.buildingFilepath).parent_path()
			/ target.building->getAgentBehaviourRegistryPackageName();
	}

	filesystem::path normalizedSavePath(filesystem::path path)
	{
		error_code error;
		auto canonical = filesystem::weakly_canonical(path, error);
		if (!error) return canonical;
		auto absolute = filesystem::absolute(path, error);
		if (!error) path = std::move(absolute);
		return path.lexically_normal();
	}

	bool saveDocuments(vector<BuildingDocumentSaveTarget> const& targets,
		bool forceBuildingSave, string* diagnostic)
	{
		if (diagnostic) diagnostic->clear();
		struct RegistrySave
		{
			shared_ptr<core::AgentTagRegistry> registry;
			filesystem::path path;
		};
		struct RegistryCopy
		{
			BuildingDocumentSaveTarget const* target{ nullptr };
			shared_ptr<core::AgentTagRegistry> source;
			shared_ptr<core::AgentTagRegistry> copy;
			filesystem::path destination;
			bool buildingWasModified{ false };
			bool committed{ false };
		};
		struct BehaviourRegistrySave
		{
			shared_ptr<core::AgentBehaviourRegistry> registry;
			filesystem::path package;
		};
		struct BehaviourRegistryCopy
		{
			BuildingDocumentSaveTarget const* target{ nullptr };
			shared_ptr<core::AgentBehaviourRegistry> source;
			shared_ptr<core::AgentBehaviourRegistry> copy;
			filesystem::path sourcePackage;
			filesystem::path destination;
			bool buildingWasModified{ false };
			bool committed{ false };
		};
		vector<RegistrySave> registries;
		map<core::AgentTagRegistry const*, size_t> registryIndices;
		vector<RegistryCopy> copies;
		vector<BehaviourRegistrySave> behaviourRegistries;
		map<core::AgentBehaviourRegistry const*, size_t> behaviourRegistryIndices;
		vector<BehaviourRegistryCopy> behaviourCopies;
		map<filesystem::path, BuildingDocumentSaveTarget const*> copyDestinations;
		vector<BuildingDocumentSaveTarget const*> buildings;
		map<core::Building const*, filesystem::path> buildingPaths;

		auto refuse = [diagnostic](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};

		auto pathIsOccupied = [&refuse](filesystem::path const& path,
			bool& occupied)
		{
			error_code error;
			auto const status = filesystem::symlink_status(path, error);
			if (!error)
			{
				occupied = status.type() != filesystem::file_type::not_found;
				return true;
			}
			if (error == errc::no_such_file_or_directory)
			{
				occupied = false;
				return true;
			}
			return refuse("Could not inspect dependency copy destination: "
				+ error.message());
		};

		// Validate and plan the complete ordering before writing any document.
		// This includes every cross-directory copy collision, so a refused Save As
		// cannot save the source registry or mutate either Building namespace.
		for (auto const& target : targets)
		{
			if (!target.building) return refuse("There is no Building document to save");
			auto const saveBuilding = forceBuildingSave || buildingDocumentIsModified(target);
			if (saveBuilding)
			{
				if (target.buildingFilepath.empty())
					return refuse("The Building has no file path");
				auto const path = normalizedSavePath(target.buildingFilepath);
				auto const [entry, inserted] = buildingPaths.emplace(
					target.building.get(), path);
				if (!inserted && entry->second != path)
					return refuse("The same Building was given more than one save path");
				if (inserted) buildings.push_back(&target);

				if (target.building->hasAttachedAgentTagRegistry())
				{
					auto const sourcePath = registrySavePath(target);
					if (sourcePath.empty())
						return refuse("The attached Agent tag registry has no file path");
					auto const normalizedSource = normalizedSavePath(sourcePath);
					auto const destination = path.parent_path()
						/ target.building->getAgentTagRegistryFilename();
					if (normalizedSource == path)
						return refuse("The Building and its Agent tag registry cannot use the same file path");
					if (normalizedSource.parent_path() != destination.parent_path())
					{
						if (destination == path)
							return refuse("The Building and its Agent tag registry copy cannot use the same file path");
						bool occupied{ false };
						if (!pathIsOccupied(destination, occupied)) return false;
						if (occupied)
							return refuse("Agent tag registry copy already exists: "
								+ destination.string());
						if (!copyDestinations.emplace(destination, &target).second)
							return refuse("More than one dependency copy targets "
								+ destination.string());
						copies.push_back({ &target,
							target.building->getAgentTagRegistry(), {}, destination,
							target.building->isModified(), false });
					}
				}
				if (target.building->hasAttachedAgentBehaviourRegistry())
				{
					auto const sourcePackage = behaviourPackageSavePath(target);
					if (sourcePackage.empty())
						return refuse("The attached Agent behaviour registry has no package path");
					auto const normalizedSource = normalizedSavePath(sourcePackage);
					auto const destination = path.parent_path()
						/ target.building->getAgentBehaviourRegistryPackageName();
					if (normalizedSource.parent_path() != destination.parent_path())
					{
						bool occupied{ false };
						if (!pathIsOccupied(destination, occupied)) return false;
						if (occupied)
							return refuse("Agent behaviour registry package copy already exists: "
								+ destination.string());
						if (!copyDestinations.emplace(destination, &target).second)
							return refuse("More than one dependency copy targets "
								+ destination.string());
						behaviourCopies.push_back({ &target,
							target.building->getAgentBehaviourRegistry(), {},
							normalizedSource, destination,
							target.building->isModified(), false });
					}
				}
			}

			if (target.building->hasAttachedAgentTagRegistry()
				&& attachedAgentTagRegistryIsModified(target.building))
			{
				auto const registryPath = registrySavePath(target);
				if (registryPath.empty())
					return refuse("The attached Agent tag registry has no file path");
				auto const normalized = normalizedSavePath(registryPath);
				auto const& registry = target.building->getAgentTagRegistry();
				auto const [entry, inserted] = registryIndices.emplace(
					registry.get(), registries.size());
				if (inserted) registries.push_back({ registry, normalized });
				else if (registries[entry->second].path != normalized)
					return refuse("The same Agent tag registry was given more than one save path");
			}
			if (target.building->hasAttachedAgentBehaviourRegistry()
				&& attachedAgentBehaviourRegistryIsModified(target.building))
			{
				auto const package = behaviourPackageSavePath(target);
				if (package.empty())
					return refuse("The attached Agent behaviour registry has no package path");
				auto const normalized = normalizedSavePath(package);
				auto const& registry = target.building->getAgentBehaviourRegistry();
				auto const [entry, inserted] = behaviourRegistryIndices.emplace(
					registry.get(), behaviourRegistries.size());
				if (inserted) behaviourRegistries.push_back({ registry, normalized });
				else if (behaviourRegistries[entry->second].package != normalized)
					return refuse("The same Agent behaviour registry was given more than one package path");
			}
		}

		// Save All is deliberately phased: no Building is written until every
		// dirty source registry and every required independent copy has succeeded.
		// Claim copy destinations before changing source save state, so even a
		// destination created after preflight leaves the source untouched.
		auto discardCopy = [](RegistryCopy& entry)
		{
			if (!entry.copy || entry.committed) return;
			(void)core::unloadAgentTagRegistryDocumentIfUnused(entry.copy, true);
			error_code ignored;
			filesystem::remove(entry.destination, ignored);
			entry.copy.reset();
		};
		auto discardBehaviourCopy = [](BehaviourRegistryCopy& entry)
		{
			if (!entry.copy || entry.committed) return;
			(void)core::unloadAgentBehaviourRegistryDocumentIfUnused(entry.copy, true);
			error_code ignored;
			filesystem::remove_all(entry.destination, ignored);
			entry.copy.reset();
		};
		auto discardUncommittedCopies = [&]()
		{
			for (auto& entry : copies) discardCopy(entry);
			for (auto& entry : behaviourCopies) discardBehaviourCopy(entry);
		};

		for (auto& entry : copies)
		{
			try
			{
				entry.copy = core::copyAgentTagRegistryDocument(
					*entry.source, entry.destination);
			}
			catch (std::exception const& error)
			{
				discardUncommittedCopies();
				return refuse("Could not copy Agent tag registry: "
					+ string(error.what()));
			}
		}

		for (auto& entry : behaviourCopies)
		{
			try
			{
				entry.copy = core::copyAgentBehaviourRegistryDocument(
					*entry.source, entry.sourcePackage, entry.destination);
			}
			catch (std::exception const& error)
			{
				discardUncommittedCopies();
				return refuse("Could not copy Agent behaviour registry package: "
					+ string(error.what()));
			}
		}

		// Shared dirty source registries are written once, after all no-clobber
		// copy installations have succeeded and before any dependent Building.
		for (auto const& entry : registries)
		{
			string registryDiagnostic;
			if (!saveAgentTagRegistry(entry.registry, entry.path.string(),
				&registryDiagnostic))
			{
				discardUncommittedCopies();
				return refuse(std::move(registryDiagnostic));
			}
		}

		for (auto const& entry : behaviourRegistries)
		{
			string registryDiagnostic;
			if (!saveAgentBehaviourRegistry(entry.registry, entry.package.string(),
				&registryDiagnostic))
			{
				discardUncommittedCopies();
				return refuse(std::move(registryDiagnostic));
			}
		}

		for (auto const* target : buildings)
		{
			auto copy = find_if(copies.begin(), copies.end(),
				[target](RegistryCopy const& entry) { return entry.target == target; });
			auto behaviourCopy = find_if(behaviourCopies.begin(), behaviourCopies.end(),
				[target](BehaviourRegistryCopy const& entry) { return entry.target == target; });
			bool attachedCopy{ false };
			bool attachedBehaviourCopy{ false };
			try
			{
				if (copy != copies.end())
				{
					target->building->replaceAgentTagRegistryWithIndependentCopy(
						copy->destination.filename().string(), copy->copy);
					attachedCopy = true;
				}
				if (behaviourCopy != behaviourCopies.end())
				{
					target->building->replaceAgentBehaviourRegistryWithIndependentCopy(
						behaviourCopy->destination.filename().string(), behaviourCopy->copy);
					attachedBehaviourCopy = true;
				}
				target->building->saveTo(target->buildingFilepath);
			}
			catch (std::exception const& error)
			{
				try
				{
					if (attachedBehaviourCopy)
						target->building->replaceAgentBehaviourRegistryWithIndependentCopy(
							behaviourCopy->sourcePackage.filename().string(),
							behaviourCopy->source);
					if (attachedCopy)
						target->building->replaceAgentTagRegistryWithIndependentCopy(
							copy->destination.filename().string(), copy->source);
					bool const wasModified = attachedBehaviourCopy
						? behaviourCopy->buildingWasModified
						: (attachedCopy ? copy->buildingWasModified : true);
					if (!wasModified) target->building->markSaved();
				}
				catch (...) {}
				discardUncommittedCopies();
				return refuse("Could not save Building: " + string(error.what()));
			}

			// Once the Building reaches disk, its adjacent copies are committed and
			// must not be removed by cleanup for a later independent save failure.
			if (copy != copies.end()) copy->committed = true;
			if (behaviourCopy != behaviourCopies.end()) behaviourCopy->committed = true;
			if (target->buildingHistory)
			{
				if (copy != copies.end() || behaviourCopy != behaviourCopies.end())
					target->buildingHistory->clear();
				target->buildingHistory->markSaved();
			}
			if (copy != copies.end())
			{
				releaseRegistryIfUnused(copy->source);
				core::addLogMessage("Tags", 0, core::LogLevel::Info,
					"Copied Agent tag registry to " + copy->destination.string());
			}
			if (behaviourCopy != behaviourCopies.end())
			{
				(void)core::unloadAgentBehaviourRegistryDocumentIfUnused(
					behaviourCopy->source);
				core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
					"Copied Agent behaviour registry package to "
					+ behaviourCopy->destination.string());
			}
			core::addLogMessage("File", 0, core::LogLevel::Info,
				"Saved Building to " + target->buildingFilepath);
		}
		if (!copies.empty()) resetTagsPanelState();
		if (!behaviourCopies.empty()) resetBehavioursPanelState();
		return true;
	}
}

bool saveBuildingDocument(BuildingDocumentSaveTarget const& target,
	string* diagnostic)
{
	return saveDocuments({ target }, true, diagnostic);
}

bool saveAllDocuments(vector<BuildingDocumentSaveTarget> const& targets,
	string* diagnostic)
{
	return saveDocuments(targets, false, diagnostic);
}

string unsavedDocumentPromptText(BuildingDocumentSaveTarget const& target)
{
	if (!target.building) return "There are no unsaved documents.";
	ostringstream text;
	text << "Save unsaved documents?";
	if (buildingDocumentIsModified(target))
	{
		auto label = filesystem::path(target.buildingFilepath).filename().string();
		if (label.empty()) label = target.building->getName() + " (not yet saved)";
		text << "\n- Building: " << label;
	}
	if (attachedAgentTagRegistryIsModified(target.building))
		text << "\n- Agent tag registry: "
			<< target.building->getAgentTagRegistryFilename();
	if (attachedAgentBehaviourRegistryIsModified(target.building))
		text << "\n- Agent behaviour registry: "
			<< target.building->getAgentBehaviourRegistryPackageName();
	return text.str();
}

void resetTagsPanelState()
{
	gTagNameEdits.clear();
	gTagDisplayColourEdits.clear();
	gTagColourEdits.clear();
	gTagWalkSpeedEdits.clear();
	gTagHeightEdits.clear();
	gTagSearch.fill('\0');
	cancelPendingAgentTagDelete();
	cancelPendingAgentTagRegistryChange();
	gAddingTag = false;
	gFocusAddTag = false;
	loadIntoBuffer(gNewTagName, "");
	gAddTagDiagnostic.clear();
}

void forgetAgentTagRegistryDocument(shared_ptr<core::AgentTagRegistry> const& registry)
{
	// Callers use this only after an explicit close/discard decision. Attached
	// registries remain manager-owned through their Building; an unreferenced
	// dirty registry may therefore be released here without pretending it saved.
	if (registry)
	{
		gRegistryHistories.erase(registry->getUuid());
		(void)core::unloadAgentTagRegistryDocumentIfUnused(registry, true);
	}
	resetTagsPanelState();
}

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
		return renderAttachedRegistry(building, buildingFilepath, selectRegistryPath);

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
			(void)agentTagRegistryDocumentHistory(registry);
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
		(void)agentTagRegistryDocumentHistory(registry);
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
