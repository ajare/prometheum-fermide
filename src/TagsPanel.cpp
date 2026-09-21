#include "TagsPanel.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <map>
#include <utility>

#include "DocumentEdit.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Building.h"
#include "core/Log.h"
#include "core/YamlSerializer.h"
#include "imgui/IconsFontAwesome5.h"
#include "imgui/imgui.h"

using namespace std;

namespace
{
	constexpr size_t NameBufferSize{ core::AgentTag::MaxNameCharacters + 1 };

	struct TagNameEdit
	{
		array<char, NameBufferSize> text{};
		bool editing{ false };
		string previous;
		string diagnostic;
	};

	map<string, DocumentHistory> gRegistryHistories;
	map<uint64_t, TagNameEdit> gTagNameEdits;
	bool gAddingTag{ false };
	bool gFocusAddTag{ false };
	array<char, NameBufferSize> gNewTagName{};
	string gAddTagDiagnostic;

	void loadIntoBuffer(array<char, NameBufferSize>& buffer, string const& value)
	{
		strncpy(buffer.data(), value.c_str(), buffer.size() - 1);
		buffer[buffer.size() - 1] = '\0';
	}

	optional<DocumentSnapshot> captureRegistrySnapshot(
		shared_ptr<core::AgentTagRegistry> const& registry)
	{
		if (!registry) return nullopt;
		try
		{
			auto serializer = core::YamlSerializer::toString();
			core::SerializationWorkData workData;
			workData.markSerializedUnmodified = false;
			registry->serialize(*serializer, workData);
			serializer->serialize();
			return agentTagRegistryDocumentHistory(registry).capture(
				serializer->getSerializedString());
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

	void renderTagNameEditor(shared_ptr<core::AgentTagRegistry> const& registry,
		core::AgentTagId id)
	{
		auto& edit = gTagNameEdits[id.value];
		if (!edit.editing) loadIntoBuffer(edit.text, registry->getAgentTagName(id));

		ImGui::TextUnformatted("#");
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetNextItemWidth(-1.0f);
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
		if (gFocusAddTag)
		{
			ImGui::SetKeyboardFocusHere(0);
			gFocusAddTag = false;
		}
		ImGui::TextUnformatted("#");
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetNextItemWidth(-1.0f);
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
	}

	void renderAttachedRegistry(shared_ptr<core::Building> const& building,
		string const& buildingFilepath)
	{
		auto const& registry = building->getAgentTagRegistry();
		if (!registry)
		{
			ImGui::TextDisabled("The referenced Agent tag registry is not loaded.");
			return;
		}

		ImGui::TextUnformatted("Agent tag registry");
		ImGui::SameLine();
		ImGui::Text("%s", building->getAgentTagRegistryFilename().c_str());
		ImGui::TextDisabled("UUID %s", registry->getUuid().c_str());

		auto& history = agentTagRegistryDocumentHistory(registry);
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
		ImGui::BeginDisabled(!history.canUndo());
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
		ImGui::BeginDisabled(!history.canRedo());
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
		for (auto const id : registry->getAgentTagIdsAlphabetically())
		{
			auto const idScope = to_string(id.value);
			ImGui::PushID(idScope.c_str());
			renderTagNameEditor(registry, id);
			auto const found = gTagNameEdits.find(id.value);
			if (found != gTagNameEdits.end() && !found->second.diagnostic.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
					found->second.diagnostic.c_str());
			ImGui::PopID();
		}
		if (gAddingTag) renderTagAddRow(registry);

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
	}
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
	if (!registry->renameAgentTag(id, name, &diagnostic)) return false;
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
	auto undo = captureRegistrySnapshot(registry);
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
	auto current = captureRegistrySnapshot(registry);
	if (!current) return false;
	auto& history = agentTagRegistryDocumentHistory(registry);
	try
	{
		auto restore = [&registry](DocumentSnapshot const& target)
		{
			auto serializer = core::YamlSerializer::fromString(target.yaml);
			serializer->deserialize();
			core::SerializationWorkData workData;
			return registry->deserialize(*serializer, workData);
		};
		auto const restored = redo
			? history.redo(std::move(current), restore)
			: history.undo(std::move(current), restore);
		if (!restored) return false;
		if (history.isModified()) registry->markModified();
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

void resetTagsPanelState()
{
	gTagNameEdits.clear();
	gAddingTag = false;
	gFocusAddTag = false;
	loadIntoBuffer(gNewTagName, "");
	gAddTagDiagnostic.clear();
}

void forgetAgentTagRegistryDocument(shared_ptr<core::AgentTagRegistry> const& registry)
{
	if (registry) gRegistryHistories.erase(registry->getUuid());
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
	if (building->hasAgentTagRegistryReference())
		return refuse("This Building already has an Agent tag registry");
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
	{
		renderAttachedRegistry(building, buildingFilepath);
		return false;
	}

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
