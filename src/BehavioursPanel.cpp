#include "BehavioursPanel.h"

#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#include "DocumentEdit.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/World.h"
#include "core/Log.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"
#include "imgui/IconsFontAwesome5.h"
#include "imgui/imgui.h"

using namespace std;

namespace
{
	vector<core::AgentBehaviourReloadDiagnostic> gReloadDiagnostics;
	map<string, DocumentHistory> gBehaviourRegistryHistories;
	map<weak_ptr<void const>, DocumentHistory,
		owner_less<weak_ptr<void const>>> gBehaviourWorldHistories;

	struct PendingAgentBehaviourDelete
	{
		weak_ptr<core::AgentBehaviourRegistry> registry;
		core::AgentBehaviourId id{};
		string consequence;
		uint64_t loadedAgentCount{ 0 };
		bool active{ false };
		bool openRequested{ false };
	};
	PendingAgentBehaviourDelete gPendingBehaviourDelete;

	struct BehaviourWorldSnapshot
	{
		core::World* world{ nullptr };
		weak_ptr<void const> lifetime;
		string yaml;
		bool modified{ false };
		bool paused{ false };
	};

	struct BehaviourRegistrySnapshotContext : DocumentSnapshotContext
	{
		weak_ptr<core::AgentBehaviourRegistry> registry;
		core::AgentBehaviourId affectedBehaviour{};
		vector<BehaviourWorldSnapshot> worlds;

		bool isRestorable() const override
		{
			auto loaded = registry.lock();
			if (!loaded) return false;
			for (auto const& item : worlds)
				if (item.lifetime.expired()
					|| !loaded->hasLoadedWorld(item.world)) return false;
			return true;
		}
	};

	string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	optional<DocumentSnapshot> captureBehaviourRegistrySnapshot(
		shared_ptr<core::AgentBehaviourRegistry> const& registry,
		vector<shared_ptr<core::World>> const& participants = {},
		core::AgentBehaviourId affectedBehaviour = {})
	{
		if (!registry) return nullopt;
		try
		{
			auto writer = core::YamlSerializer::toString();
			core::SerializationWorkData work;
			work.markSerializedUnmodified = false;
			registry->serialize(*writer, work);
			writer->serialize();
			auto snapshot = agentBehaviourRegistryDocumentHistory(registry).capture(
				writer->getSerializedString());
			if (!participants.empty())
			{
				auto context = make_shared<BehaviourRegistrySnapshotContext>();
				context->registry = registry;
				context->affectedBehaviour = affectedBehaviour;
				for (auto const& world : participants)
					context->worlds.push_back({ world.get(),
						world->getLifetimeToken(), serializeWorld(*world),
						world->isModified(), world->isSimulationPaused() });
				snapshot.context = std::move(context);
			}
			return snapshot;
		}
		catch (...) { return nullopt; }
	}

	struct PendingAgentBehaviourRegistryChange
	{
		weak_ptr<core::World> world;
		string worldFilepath;
		string packageDirectory;
		string consequence;
		bool detach{ false };
		bool active{ false };
		bool openRequested{ false };
	};
	PendingAgentBehaviourRegistryChange gPendingRegistryChange;

	string registryChangeConsequence(core::World const& world, bool detach,
		string const& packageDirectory)
	{
		auto const assignments = world.getAgentBehaviourAssignmentCount();
		string result = "This destructive action will clear all "
			+ to_string(assignments) + " Agent behaviour assignment"
			+ (assignments == 1 ? "" : "s")
			+ " and configuration" + (assignments == 1 ? "" : "s") + ".\n";
		if (detach)
			result += "It will detach " + world.getAgentBehaviourRegistryPackageName() + ". ";
		else result += "It will replace the current reference with "
			+ filesystem::path(packageDirectory).filename().string() + ". ";
		result += "Registry package files will not be deleted, renamed, or rewritten.";
		return result;
	}

	filesystem::path attachedPackagePath(core::World const& world,
		string const& worldFilepath)
	{
		return filesystem::path(worldFilepath).parent_path()
			/ world.getAgentBehaviourRegistryPackageName();
	}

	void releaseRegistryIfUnused(
		shared_ptr<core::AgentBehaviourRegistry> const& registry)
	{
		if (registry)
			(void)core::unloadAgentBehaviourRegistryDocumentIfUnused(registry);
	}

	void appendSchemaField(string& output,
		core::AgentBehaviourSchemaField const& field, string const& indent)
	{
		output += indent + field.name + ": ";
		switch (field.type)
		{
		case core::AgentBehaviourSchemaType::List:
			output += "list of ";
			if (!field.children.empty()) appendSchemaField(output, field.children.front(), "");
			else output += "?";
			output += "\n";
			break;
		case core::AgentBehaviourSchemaType::Record:
			output += "record\n";
			for (auto const& child : field.children)
				appendSchemaField(output, child, indent + "  ");
			break;
		default:
			output += core::agentBehaviourSchemaTypeName(field.type);
			output += "\n";
			break;
		}
	}

	bool renderAttachedRegistry(shared_ptr<core::World> const& world,
		string const& worldFilepath,
		AgentBehaviourRegistryPathSelector const& selectPackageDirectory)
	{
		auto const& registry = world->getAgentBehaviourRegistry();
		if (!registry)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
				"Agent behaviour dependency unavailable");
			ImGui::TextWrapped("%s",
				world->getAgentBehaviourDependencyDiagnostic().c_str());

			string selectDiagnostic;
			auto canSelect = canSelectAgentBehaviourRegistry(
				world, worldFilepath, &selectDiagnostic);
			if (!selectPackageDirectory)
			{
				canSelect = false;
				selectDiagnostic = "Registry package selection is unavailable";
			}
			ImGui::BeginDisabled(!canSelect);
			auto const repairClicked = ImGui::Button("Repair or replace registry");
			ImGui::EndDisabled();
			if (!canSelect && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", selectDiagnostic.c_str());
			ImGui::SameLine();
			ImGui::BeginDisabled(!world->isSimulationPaused());
			auto const detachClicked = ImGui::Button("Detach registry");
			ImGui::EndDisabled();

			if (repairClicked)
			{
				try
				{
					auto selectedPath = selectPackageDirectory();
					if (selectedPath)
					{
						string diagnostic;
						if (commitAgentBehaviourRegistrySwitch(world,
							worldFilepath, *selectedPath, diagnostic)) return true;
						if (world->getAgentBehaviourAssignmentCount() != 0
							&& diagnostic.find("confirmed destructive action") != string::npos)
							requestAgentBehaviourRegistrySwitch(world,
								worldFilepath, *selectedPath);
						else if (!diagnostic.empty())
							core::addLogMessage("Behaviours", 0,
								core::LogLevel::Warning, diagnostic);
					}
				}
				catch (exception const& error)
				{
					core::addLogMessage("Behaviours", 0,
						core::LogLevel::Error, error.what());
				}
			}
			if (detachClicked)
			{
				if (world->getAgentBehaviourAssignmentCount() != 0)
					requestAgentBehaviourRegistryDetach(world);
				else
				{
					string diagnostic;
					if (commitAgentBehaviourRegistryDetach(world, diagnostic)) return true;
					if (!diagnostic.empty()) core::addLogMessage("Behaviours", 0,
						core::LogLevel::Warning, diagnostic);
				}
			}
			return false;
		}

		ImGui::TextUnformatted("Agent behaviour registry package");
		ImGui::SameLine();
		ImGui::Text("%s", world->getAgentBehaviourRegistryPackageName().c_str());
		ImGui::TextDisabled("UUID %s", registry->getUuid().c_str());
		ImGui::TextDisabled("Package revision %llu",
			static_cast<unsigned long long>(registry->getPackageRevision()));
		if (!world->agentBehaviourConfigurationsAreValid())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
				"Schema reconciliation required");
			ImGui::TextWrapped("%s",
				world->getAgentBehaviourDependencyDiagnostic().c_str());
		}

		string switchDiagnostic;
		auto canSwitch = canSelectAgentBehaviourRegistry(
			world, worldFilepath, &switchDiagnostic);
		if (!selectPackageDirectory)
		{
			canSwitch = false;
			switchDiagnostic = "Registry package selection is unavailable";
		}
		ImGui::BeginDisabled(!canSwitch);
		auto const switchClicked = ImGui::Button("Switch registry");
		ImGui::EndDisabled();
		if (!canSwitch && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", switchDiagnostic.c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(!world->isSimulationPaused());
		auto const detachClicked = ImGui::Button("Detach registry");
		ImGui::EndDisabled();

		if (switchClicked)
		{
			optional<string> selectedPath;
			try { selectedPath = selectPackageDirectory(); }
			catch (exception const& error)
			{
				core::addLogMessage("Behaviours", 0, core::LogLevel::Error, error.what());
			}
			if (selectedPath)
			{
				string diagnostic;
				if (commitAgentBehaviourRegistrySwitch(world, worldFilepath,
					*selectedPath, diagnostic)) return true;
				if (world->getAgentBehaviourAssignmentCount() != 0
					&& diagnostic.find("confirmed destructive action") != string::npos)
					requestAgentBehaviourRegistrySwitch(world,
						worldFilepath, *selectedPath);
				else if (!diagnostic.empty())
					core::addLogMessage("Behaviours", 0, core::LogLevel::Warning, diagnostic);
			}
		}
		if (detachClicked)
		{
			if (world->getAgentBehaviourAssignmentCount() != 0)
				requestAgentBehaviourRegistryDetach(world);
			else
			{
				string diagnostic;
				if (commitAgentBehaviourRegistryDetach(world, diagnostic)) return true;
				if (!diagnostic.empty())
					core::addLogMessage("Behaviours", 0, core::LogLevel::Warning, diagnostic);
			}
		}

		string editDiagnostic;
		auto const definitionEditsAllowed
			= registry->definitionEditsAreAllowed(&editDiagnostic);
		if (registry->isModified())
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "Modified");
		}

		auto& registryHistory = agentBehaviourRegistryDocumentHistory(registry);
		ImGui::BeginDisabled(!registryHistory.canUndo());
		if (ImGui::Button(ICON_FA_UNDO "##BehaviourRegistryUndo"))
		{
			string diagnostic;
			if (!restoreAgentBehaviourRegistrySnapshot(registry, false, &diagnostic)
				&& !diagnostic.empty())
				core::addLogMessage("Behaviours", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!registryHistory.canRedo());
		if (ImGui::Button(ICON_FA_REDO "##BehaviourRegistryRedo"))
		{
			string diagnostic;
			if (!restoreAgentBehaviourRegistrySnapshot(registry, true, &diagnostic)
				&& !diagnostic.empty())
				core::addLogMessage("Behaviours", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!registry->isModified());
		if (ImGui::Button(ICON_FA_SAVE " Save registry"))
		{
			string diagnostic;
			if (!saveAgentBehaviourRegistry(registry,
				attachedPackagePath(*world, worldFilepath).string(), &diagnostic))
				core::addLogMessage("Behaviours", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Persist reconciled schema history");
		ImGui::SameLine();
		ImGui::BeginDisabled(registry->isModified() || !definitionEditsAllowed);
		if (ImGui::Button(ICON_FA_SYNC " Reload registry"))
		{
			string diagnostic;
			if (!reloadAgentBehaviourRegistry(registry,
				attachedPackagePath(*world, worldFilepath).string(), &diagnostic))
				core::addLogMessage("Behaviours", 0, core::LogLevel::Error, diagnostic);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			if (registry->isModified())
				ImGui::SetTooltip("Save or discard registry changes before reloading");
			else if (!definitionEditsAllowed)
				ImGui::SetTooltip("%s", editDiagnostic.c_str());
			else ImGui::SetTooltip("Reload source and helpers atomically after preflighting every affected Agent");
		}

		if (!gReloadDiagnostics.empty())
		{
			ImGui::SeparatorText("Reload diagnostics");
			ImGui::TextWrapped("The candidate package was not adopted (%u diagnostic%s).",
				static_cast<unsigned>(gReloadDiagnostics.size()),
				gReloadDiagnostics.size() == 1 ? "" : "s");
			for (auto const& item : gReloadDiagnostics)
			{
				if (item.scope == core::AgentBehaviourReloadDiagnosticScope::Agent)
				{
					if (!item.agentName.empty())
						ImGui::BulletText("World %s / Agent %s (%llu) / %s",
							item.worldName.c_str(), item.agentName.c_str(),
							static_cast<unsigned long long>(item.agent.value),
							item.moduleName.c_str());
					else ImGui::BulletText("World %s", item.worldName.c_str());
				}
				else if (item.scope == core::AgentBehaviourReloadDiagnosticScope::Module)
					ImGui::BulletText("Module %s", item.moduleName.c_str());
				else ImGui::BulletText("Package");
				ImGui::Indent();
				ImGui::TextWrapped("%s", item.diagnostic.c_str());
				if (!item.traceback.empty()
					&& item.traceback != item.diagnostic)
					ImGui::TextWrapped("%s", item.traceback.c_str());
				ImGui::Unindent();
			}
		}

		auto const helperNames = registry->getHelperModuleNames();
		if (!helperNames.empty())
		{
			ImGui::SeparatorText("Helper modules");
			for (auto const& name : helperNames)
			{
				auto const* helper = registry->lookupHelperModule(name);
				if (!helper) continue;
				auto const status = helper->getModuleStatus();
				auto const statusColour = status == core::AgentBehaviourModuleStatus::Loaded
					? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
					: status == core::AgentBehaviourModuleStatus::Error
						? ImVec4(1.0f, 0.35f, 0.3f, 1.0f)
						: ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
				ImGui::BulletText("%s (%s)", name.c_str(),
					helper->getSourceModulePath().c_str());
				ImGui::SameLine();
				ImGui::TextColored(statusColour, "%s",
					core::agentBehaviourModuleStatusName(status));
				if (!helper->getModuleDiagnostic().empty()
					&& ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", helper->getModuleDiagnostic().c_str());
			}
		}

		ImGui::SeparatorText("Behaviours");
		auto const ids = registry->getBehaviourIdsAlphabetically();
		ImGui::TextDisabled("%u behaviour%s", registry->getBehaviourCount(),
			registry->getBehaviourCount() == 1 ? "" : "s");
		if (ids.empty())
		{
			ImGui::TextWrapped(
				"This package declares no behaviours yet. Add behaviour definitions and Lua source "
				"modules to its behaviours.yaml manifest, then reload.");
		}
		for (auto const id : ids)
		{
			auto const* behaviour = registry->lookupAgentBehaviour(id);
			if (!behaviour) continue;
			ImGui::PushID(to_string(id.value).c_str());
			auto const open = ImGui::TreeNode("##definition");
			ImGui::SameLine();
			ImGui::TextUnformatted(behaviour->getName().c_str());
			ImGui::SameLine();
			auto const status = behaviour->getModuleStatus();
			auto const statusColour = status == core::AgentBehaviourModuleStatus::Loaded
				? ImVec4(0.35f, 0.85f, 0.45f, 1.0f)
				: status == core::AgentBehaviourModuleStatus::Error
					? ImVec4(1.0f, 0.35f, 0.3f, 1.0f)
					: ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
			ImGui::TextColored(statusColour, "%s",
				core::agentBehaviourModuleStatusName(status));
			ImGui::SameLine();
			ImGui::BeginDisabled(!definitionEditsAllowed);
			auto const deleteClicked = ImGui::SmallButton(ICON_FA_TRASH " Delete");
			ImGui::EndDisabled();
			if (deleteClicked) requestAgentBehaviourDelete(registry, id);
			if (open)
			{
				ImGui::BulletText("Revision %llu", (unsigned long long)behaviour->getRevision());
				ImGui::BulletText("Source module %s", behaviour->getSourceModulePath().c_str());
				if (!behaviour->getModuleDiagnostic().empty())
				{
					ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Preflight diagnostic:");
					ImGui::TextWrapped("%s", behaviour->getModuleDiagnostic().c_str());
				}
				if (!behaviour->getModuleTraceback().empty())
				{
					ImGui::TextUnformatted("Traceback:");
					ImGui::TextWrapped("%s", behaviour->getModuleTraceback().c_str());
				}
				if (behaviour->getSchema().empty())
					ImGui::BulletText("Configuration: none");
				else
				{
					ImGui::TextUnformatted("Configuration schema:");
					// Render one bullet per top-level field so nested records stay readable.
					for (auto const& field : behaviour->getSchema())
					{
						string fieldText;
						appendSchemaField(fieldText, field, "");
						ImGui::Bullet();
						ImGui::SameLine();
						ImGui::TextUnformatted(fieldText.c_str());
					}
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (!definitionEditsAllowed)
			ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s",
				editDiagnostic.c_str());
		ImGui::TextDisabled(
			"Definitions and Lua source are authored externally; deletion is coordinated here and Reload re-runs protected preflight.");
		return false;
	}

	bool renderBehaviourDeleteConfirmation(
		shared_ptr<core::AgentBehaviourRegistry> const& registry)
	{
		if (gPendingBehaviourDelete.openRequested)
		{
			ImGui::OpenPopup("Delete used Agent behaviour?");
			gPendingBehaviourDelete.openRequested = false;
		}
		if (!ImGui::BeginPopupModal("Delete used Agent behaviour?", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize)) return false;
		ImGui::TextWrapped("%s", gPendingBehaviourDelete.consequence.c_str());
		bool changed{ false };
		if (ImGui::Button("Clear assignments and delete"))
		{
			string diagnostic;
			changed = confirmPendingAgentBehaviourDelete(registry, diagnostic);
			if (!changed && !diagnostic.empty())
				core::addLogMessage("Behaviours", 0, core::LogLevel::Error, diagnostic);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			cancelPendingAgentBehaviourDelete();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
		return changed;
	}

	bool renderRegistryChangeConfirmation()
	{
		if (gPendingRegistryChange.openRequested)
		{
			ImGui::OpenPopup("Clear Agent behaviour assignments?");
			gPendingRegistryChange.openRequested = false;
		}
		bool changed{ false };
		if (ImGui::BeginPopupModal("Clear Agent behaviour assignments?", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("%s", gPendingRegistryChange.consequence.c_str());
			if (ImGui::Button("Clear assignments and continue"))
			{
				string diagnostic;
				changed = confirmPendingAgentBehaviourRegistryChange(diagnostic);
				if (!changed && !diagnostic.empty())
					core::addLogMessage("Behaviours", 0,
						core::LogLevel::Error, diagnostic);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				cancelPendingAgentBehaviourRegistryChange();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		return changed;
	}
}

bool commitAgentBehaviourRegistryDetach(
	shared_ptr<core::World> const& world, string& diagnostic)
{
	diagnostic.clear();
	if (!world || !world->hasAgentBehaviourRegistryReference())
	{
		diagnostic = "There is no Agent behaviour registry to detach";
		return false;
	}
	auto undo = captureDocumentSnapshot(world);
	if (!undo)
	{
		diagnostic = "Could not capture the World before detaching its Agent behaviour registry";
		return false;
	}
	try
	{
		auto const packageName = world->getAgentBehaviourRegistryPackageName();
		auto registry = world->getAgentBehaviourRegistry();
		world->detachAgentBehaviourRegistry();
		commitDocumentEdit(std::move(undo));
		releaseRegistryIfUnused(registry);
		resetBehavioursPanelState();
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			"Detached Agent behaviour registry package " + packageName
				+ " without changing its files");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

bool commitAgentBehaviourRegistryDetachClearingAssignments(
	shared_ptr<core::World> const& world, string& diagnostic)
{
	diagnostic.clear();
	if (!world || !world->hasAgentBehaviourRegistryReference())
	{
		diagnostic = "There is no Agent behaviour registry to detach";
		return false;
	}
	auto undo = captureDocumentSnapshot(world);
	if (!undo)
	{
		diagnostic = "Could not capture the World before clearing Agent behaviour assignments and detaching its registry";
		return false;
	}
	try
	{
		auto const packageName = world->getAgentBehaviourRegistryPackageName();
		auto registry = world->getAgentBehaviourRegistry();
		world->detachAgentBehaviourRegistryAndClearAssignments();
		commitDocumentEdit(std::move(undo));
		releaseRegistryIfUnused(registry);
		resetBehavioursPanelState();
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			"Cleared all Agent behaviour assignments and configurations, then detached "
				+ packageName + " without changing its files");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

bool commitAgentBehaviourRegistrySwitch(
	shared_ptr<core::World> const& world, string const& worldFilepath,
	string const& packageDirectory, string& diagnostic)
{
	diagnostic.clear();
	if (!world)
	{
		diagnostic = "No World is open";
		return false;
	}
	auto undo = captureDocumentSnapshot(world);
	if (!undo)
	{
		diagnostic = "Could not capture the World before switching Agent behaviour registries";
		return false;
	}
	try
	{
		auto const previouslyAttached = world->hasAttachedAgentBehaviourRegistry();
		auto previousRegistry = previouslyAttached
			? world->getAgentBehaviourRegistry() : nullptr;
		auto const previousPackage = world->hasAgentBehaviourRegistryReference()
			? world->getAgentBehaviourRegistryPackageName() : string{};
		auto const previousUuid = world->hasAgentBehaviourRegistryReference()
			? world->getExpectedAgentBehaviourRegistryUuid() : string{};
		auto registry = core::selectAndAttachAgentBehaviourRegistry(
			*world, worldFilepath, packageDirectory);
		auto const referenceChanged
			= world->getAgentBehaviourRegistryPackageName() != previousPackage
				|| world->getExpectedAgentBehaviourRegistryUuid() != previousUuid;
		if (!referenceChanged && previouslyAttached)
		{
			diagnostic = "The selected Agent behaviour registry is already attached";
			return false;
		}
		// Resolving the already-persisted expected package changes only runtime
		// dependency state, so it creates no authored undo entry or dirty state.
		if (referenceChanged) commitDocumentEdit(std::move(undo));
		if (previousRegistry != registry) releaseRegistryIfUnused(previousRegistry);
		resetBehavioursPanelState();
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			string(referenceChanged ? "Switched to" : "Recovered")
				+ " Agent behaviour registry package "
				+ world->getAgentBehaviourRegistryPackageName()
				+ " (" + registry->getUuid() + ")");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

bool commitAgentBehaviourRegistrySwitchClearingAssignments(
	shared_ptr<core::World> const& world, string const& worldFilepath,
	string const& packageDirectory, string& diagnostic)
{
	diagnostic.clear();
	if (!world)
	{
		diagnostic = "No World is open";
		return false;
	}
	auto undo = captureDocumentSnapshot(world);
	if (!undo)
	{
		diagnostic = "Could not capture the World before clearing Agent behaviour assignments and switching registries";
		return false;
	}
	try
	{
		auto previousRegistry = world->hasAttachedAgentBehaviourRegistry()
			? world->getAgentBehaviourRegistry() : nullptr;
		auto registry = core::selectAndAttachAgentBehaviourRegistryClearingAssignments(
			*world, worldFilepath, packageDirectory);
		commitDocumentEdit(std::move(undo));
		if (previousRegistry != registry) releaseRegistryIfUnused(previousRegistry);
		resetBehavioursPanelState();
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			"Cleared all Agent behaviour assignments and configurations, then switched to "
				+ world->getAgentBehaviourRegistryPackageName()
				+ " (" + registry->getUuid() + ")");
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
}

void requestAgentBehaviourRegistryDetach(
	shared_ptr<core::World> const& world)
{
	if (!world || !world->hasAgentBehaviourRegistryReference()
		|| world->getAgentBehaviourAssignmentCount() == 0) return;
	gPendingRegistryChange.world = world;
	gPendingRegistryChange.consequence = registryChangeConsequence(
		*world, true, {});
	gPendingRegistryChange.detach = true;
	gPendingRegistryChange.active = true;
	gPendingRegistryChange.openRequested = true;
}

void requestAgentBehaviourRegistrySwitch(
	shared_ptr<core::World> const& world, string worldFilepath,
	string packageDirectory)
{
	if (!world || !world->hasAgentBehaviourRegistryReference()
		|| world->getAgentBehaviourAssignmentCount() == 0) return;
	gPendingRegistryChange.world = world;
	gPendingRegistryChange.worldFilepath = std::move(worldFilepath);
	gPendingRegistryChange.packageDirectory = std::move(packageDirectory);
	gPendingRegistryChange.consequence = registryChangeConsequence(*world,
		false, gPendingRegistryChange.packageDirectory);
	gPendingRegistryChange.detach = false;
	gPendingRegistryChange.active = true;
	gPendingRegistryChange.openRequested = true;
}

bool agentBehaviourRegistryChangePending(string* consequence)
{
	if (consequence) *consequence = gPendingRegistryChange.active
		? gPendingRegistryChange.consequence : string{};
	return gPendingRegistryChange.active;
}

bool confirmPendingAgentBehaviourRegistryChange(string& diagnostic)
{
	if (!gPendingRegistryChange.active)
	{
		diagnostic = "No Agent behaviour registry change is awaiting confirmation";
		return false;
	}
	auto pending = gPendingRegistryChange;
	cancelPendingAgentBehaviourRegistryChange();
	auto world = pending.world.lock();
	if (!world)
	{
		diagnostic = "The World awaiting an Agent behaviour registry change is no longer open";
		return false;
	}
	if (pending.detach)
		return commitAgentBehaviourRegistryDetachClearingAssignments(
			world, diagnostic);
	return commitAgentBehaviourRegistrySwitchClearingAssignments(world,
		pending.worldFilepath, pending.packageDirectory, diagnostic);
}

void cancelPendingAgentBehaviourRegistryChange()
{
	gPendingRegistryChange = PendingAgentBehaviourRegistryChange{};
}

bool reloadAgentBehaviourRegistry(
	shared_ptr<core::AgentBehaviourRegistry> const& registry,
	string const& packageDirectory, string* diagnostic)
{
	if (!core::reloadAgentBehaviourRegistryDocument(registry, packageDirectory,
		diagnostic, &gReloadDiagnostics))
		return false;
	auto& history = agentBehaviourRegistryDocumentHistory(registry);
	history.clear();
	history.markSaved();
	resetBehavioursPanelState();
	core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
		"Reloaded Agent behaviour registry package " + packageDirectory);
	return true;
}

bool saveAgentBehaviourRegistry(
	shared_ptr<core::AgentBehaviourRegistry> const& registry,
	string const& packageDirectory, string* diagnostic)
{
	if (diagnostic) diagnostic->clear();
	if (!registry)
	{
		if (diagnostic) *diagnostic = "There is no Agent behaviour registry to save";
		return false;
	}
	try
	{
		registry->saveTo(core::agentBehaviourRegistryManifestPath(
			packageDirectory).string());
		agentBehaviourRegistryDocumentHistory(registry).markSaved();
		core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
			"Saved Agent behaviour registry package " + packageDirectory);
		return true;
	}
	catch (exception const& error)
	{
		if (diagnostic) *diagnostic = "Could not save Agent behaviour registry: "
			+ string(error.what());
		return false;
	}
}

bool attachedAgentBehaviourRegistryIsModified(
	shared_ptr<const core::World> const& world)
{
	return world && world->hasAttachedAgentBehaviourRegistry()
		&& world->getAgentBehaviourRegistry()->isModified();
}

DocumentHistory& agentBehaviourRegistryDocumentHistory(
	shared_ptr<core::AgentBehaviourRegistry> const& registry)
{
	if (!registry) throw invalid_argument(
		"There is no Agent behaviour registry document");
	auto const [entry, inserted]
		= gBehaviourRegistryHistories.try_emplace(registry->getUuid());
	if (inserted && !registry->isModified()) entry->second.markSaved();
	return entry->second;
}

DocumentHistory& agentBehaviourWorldDocumentHistory(
	shared_ptr<core::World> const& world)
{
	if (!world) throw invalid_argument("There is no World document");
	for (auto item = gBehaviourWorldHistories.begin();
		item != gBehaviourWorldHistories.end();)
	{
		if (item->first.expired()) item = gBehaviourWorldHistories.erase(item);
		else ++item;
	}
	auto const [entry, inserted]
		= gBehaviourWorldHistories.try_emplace(world->getLifetimeToken());
	if (inserted && !world->isModified()) entry->second.markSaved();
	return entry->second;
}

uint64_t loadedAgentBehaviourUsageCount(
	core::AgentBehaviourRegistry const& registry, core::AgentBehaviourId id)
{
	return registry.getLoadedAgentBehaviourUsageCount(id);
}

string agentBehaviourDeleteConfirmationText(
	core::AgentBehaviourRegistry const& registry, core::AgentBehaviourId id)
{
	auto const usage = registry.getLoadedAgentBehaviourUsage(id);
	uint64_t total{ 0 };
	for (auto const& item : usage) total += item.agents.size();
	ostringstream text;
	text << "Delete Agent behaviour '" << registry.getBehaviourName(id) << "'?\n"
		<< total << " loaded Agent" << (total == 1 ? " uses" : "s use")
		<< " this behaviour.";
	for (auto const& item : usage)
	{
		text << "\n- " << item.world->getName();
		for (auto const& agent : item.agents)
			text << "\n  - " << agent.name << " (" << agent.id.value << ")";
	}
	text << "\nEvery listed assignment and configuration will be cleared, its runtime instance will be stopped, and manual movement controls will be restored.";
	return text.str();
}

bool commitAgentBehaviourDelete(
	shared_ptr<core::AgentBehaviourRegistry> const& registry,
	core::AgentBehaviourId id, string& diagnostic)
{
	diagnostic.clear();
	if (!registry)
	{
		diagnostic = "There is no Agent behaviour registry from which to delete a behaviour";
		return false;
	}
	vector<shared_ptr<core::World>> participants;
	vector<optional<DocumentSnapshot>> worldSnapshots;
	try
	{
		for (auto const& usage : registry->getLoadedAgentBehaviourUsage(id))
		{
			if (!usage.world) continue;
			// Loaded Worlds are owned by the editor/caller. The no-op deleter
			// provides the existing snapshot API with shared lifetime for this call.
			auto world = shared_ptr<core::World>(
				const_cast<core::World*>(usage.world), [](core::World*) {});
			auto& history = agentBehaviourWorldDocumentHistory(world);
			auto snapshot = captureDocumentSnapshot(world, history);
			if (!snapshot)
			{
				diagnostic = "Could not capture every affected World before deleting the Agent behaviour";
				return false;
			}
			participants.push_back(std::move(world));
			worldSnapshots.push_back(std::move(snapshot));
		}
	}
	catch (exception const& error)
	{
		diagnostic = error.what();
		return false;
	}
	auto registrySnapshot = captureBehaviourRegistrySnapshot(
		registry, participants, id);
	if (!registrySnapshot)
	{
		diagnostic = "Could not capture the Agent behaviour registry before deletion";
		return false;
	}

	auto const clearedCount = registry->getLoadedAgentBehaviourUsageCount(id);
	auto const used = !participants.empty();
	bool deleted = used
		? registry->deleteAgentBehaviourClearingAssignments(id, &diagnostic)
		: registry->deleteAgentBehaviour(id, &diagnostic);
	if (!deleted) return false;

	agentBehaviourRegistryDocumentHistory(registry).commit(
		std::move(registrySnapshot));
	for (size_t index = 0; index < participants.size(); ++index)
		agentBehaviourWorldDocumentHistory(participants[index]).commit(
			std::move(worldSnapshots[index]));
	core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
		"Deleted Agent behaviour and cleared " + to_string(clearedCount)
			+ " dependent assignment" + (clearedCount == 1 ? "" : "s"));
	return true;
}

void requestAgentBehaviourDelete(
	shared_ptr<core::AgentBehaviourRegistry> const& registry,
	core::AgentBehaviourId id)
{
	if (!registry) return;
	try
	{
		auto const count = loadedAgentBehaviourUsageCount(*registry, id);
		if (count == 0)
		{
			string diagnostic;
			if (!commitAgentBehaviourDelete(registry, id, diagnostic))
				core::addLogMessage("Behaviours", 0, core::LogLevel::Warning, diagnostic);
			return;
		}
		gPendingBehaviourDelete.registry = registry;
		gPendingBehaviourDelete.id = id;
		gPendingBehaviourDelete.loadedAgentCount = count;
		gPendingBehaviourDelete.consequence
			= agentBehaviourDeleteConfirmationText(*registry, id);
		gPendingBehaviourDelete.active = true;
		gPendingBehaviourDelete.openRequested = true;
	}
	catch (exception const& error)
	{
		core::addLogMessage("Behaviours", 0, core::LogLevel::Warning, error.what());
	}
}

bool agentBehaviourDeletePending(core::AgentBehaviourId* id,
	uint64_t* loadedAgentCount, string* consequence)
{
	if (id) *id = gPendingBehaviourDelete.active
		? gPendingBehaviourDelete.id : core::AgentBehaviourId{};
	if (loadedAgentCount) *loadedAgentCount = gPendingBehaviourDelete.active
		? gPendingBehaviourDelete.loadedAgentCount : 0;
	if (consequence) *consequence = gPendingBehaviourDelete.active
		? gPendingBehaviourDelete.consequence : string{};
	return gPendingBehaviourDelete.active;
}

bool confirmPendingAgentBehaviourDelete(
	shared_ptr<core::AgentBehaviourRegistry> const& registry, string& diagnostic)
{
	if (!gPendingBehaviourDelete.active)
	{
		diagnostic = "No Agent behaviour deletion is awaiting confirmation";
		return false;
	}
	auto expected = gPendingBehaviourDelete.registry.lock();
	auto const id = gPendingBehaviourDelete.id;
	cancelPendingAgentBehaviourDelete();
	if (!registry || registry != expected)
	{
		diagnostic = "The pending Agent behaviour deletion belongs to another registry";
		return false;
	}
	return commitAgentBehaviourDelete(registry, id, diagnostic);
}

void cancelPendingAgentBehaviourDelete()
{
	gPendingBehaviourDelete = PendingAgentBehaviourDelete{};
}

bool restoreAgentBehaviourRegistrySnapshot(
	shared_ptr<core::AgentBehaviourRegistry> const& registry, bool redo,
	string* diagnostic)
{
	if (diagnostic) diagnostic->clear();
	if (!registry)
	{
		if (diagnostic) *diagnostic = "There is no Agent behaviour registry to restore";
		return false;
	}
	string pauseDiagnostic;
	if (!registry->definitionEditsAreAllowed(&pauseDiagnostic))
	{
		if (diagnostic) *diagnostic = pauseDiagnostic;
		return false;
	}
	auto& history = agentBehaviourRegistryDocumentHistory(registry);
	auto const& entries = redo ? history.redoEntries() : history.undoEntries();
	if (entries.empty()) return false;
	auto targetContext = dynamic_pointer_cast<BehaviourRegistrySnapshotContext>(
		entries.back().context);
	vector<shared_ptr<core::World>> participants;
	if (targetContext)
	{
		for (auto const& item : targetContext->worlds)
		{
			if (item.lifetime.expired() || !registry->hasLoadedWorld(item.world))
			{
				if (diagnostic) *diagnostic
					= "An affected World is no longer available for coordinated undo";
				return false;
			}
			participants.emplace_back(item.world, [](core::World*) {});
		}
	}
	auto current = captureBehaviourRegistrySnapshot(registry, participants,
		targetContext ? targetContext->affectedBehaviour
			: core::AgentBehaviourId{});
	if (!current) return false;
	vector<optional<DocumentSnapshot>> worldCurrents;
	for (auto const& world : participants)
		worldCurrents.push_back(captureDocumentSnapshot(world,
			agentBehaviourWorldDocumentHistory(world)));
	if (any_of(worldCurrents.begin(), worldCurrents.end(),
		[](auto const& snapshot) { return !snapshot; })) return false;

	try
	{
		auto restore = [&registry](DocumentSnapshot const& target)
		{
			// Parse every target first. Candidate resolution validates every restored
			// assignment before any live document changes.
			auto replacement = core::AgentBehaviourRegistry::create();
			auto registryReader = core::YamlSerializer::fromString(target.yaml);
			registryReader->deserialize();
			core::SerializationWorkData registryWork;
			if (!replacement->deserialize(*registryReader, registryWork)) return false;
			auto context = dynamic_pointer_cast<BehaviourRegistrySnapshotContext>(
				target.context);
			vector<shared_ptr<core::World>> candidates;
			if (context)
			{
				for (auto const& item : context->worlds)
				{
					auto candidate = make_shared<core::World>("Loading", 1, 1);
					auto reader = core::YamlSerializer::fromString(item.yaml);
					reader->deserialize();
					core::SerializationWorkData work;
					if (!candidate->deserialize(*reader, work)) return false;
					candidate->resolveAgentBehaviourRegistry(replacement);
					candidates.push_back(std::move(candidate));
				}
			}

			auto liveReader = core::YamlSerializer::fromString(target.yaml);
			liveReader->deserialize();
			core::SerializationWorkData liveRegistryWork;
			if (!registry->deserialize(*liveReader, liveRegistryWork)) return false;
			if (context)
			{
				for (auto const& item : context->worlds)
				{
					auto reader = core::YamlSerializer::fromString(item.yaml);
					reader->deserialize();
					core::SerializationWorkData work;
					if (!item.world->deserialize(*reader, work)) return false;
					item.world->resolveAgentBehaviourRegistry(registry);
					if (item.modified) item.world->markModified();
					if (item.paused) item.world->pauseSimulation();
				}
			}
			return true;
		};
		auto const restored = redo
			? history.redo(std::move(current), restore)
			: history.undo(std::move(current), restore);
		if (!restored) return false;
		for (size_t index = 0; index < participants.size(); ++index)
		{
			auto& worldHistory
				= agentBehaviourWorldDocumentHistory(participants[index]);
			auto shiftOnly = [](DocumentSnapshot const&) { return true; };
			auto shifted = redo
				? worldHistory.redo(std::move(worldCurrents[index]), shiftOnly)
				: worldHistory.undo(std::move(worldCurrents[index]), shiftOnly);
			if (!shifted) throw runtime_error(
				"Could not synchronize an affected World history");
		}
		if (history.isModified()) registry->markModified();
		else registry->markUnmodified();
		return true;
	}
	catch (exception const& error)
	{
		if (diagnostic) *diagnostic
			= "Could not restore Agent behaviour deletion: " + string(error.what());
		return false;
	}
}

void resetBehavioursPanelState()
{
	gReloadDiagnostics.clear();
	cancelPendingAgentBehaviourDelete();
	cancelPendingAgentBehaviourRegistryChange();
}

void forgetAgentBehaviourRegistryDocument(
	shared_ptr<core::AgentBehaviourRegistry> const& registry)
{
	// Callers use this only after an explicit close/discard decision. Attached
	// registries remain manager-owned through their World; an unreferenced
	// dirty registry may therefore be released here without pretending it saved.
	if (registry)
	{
		gBehaviourRegistryHistories.erase(registry->getUuid());
		(void)core::unloadAgentBehaviourRegistryDocumentIfUnused(registry, true);
	}
	resetBehavioursPanelState();
}

bool canCreateAgentBehaviourRegistry(
	shared_ptr<const core::World> const& world,
	string const& worldFilepath, string* diagnostic)
{
	auto refuse = [diagnostic](string message)
	{
		if (diagnostic) *diagnostic = std::move(message);
		return false;
	};
	if (!world) return refuse("No World is open");
	if (!world->isSimulationPaused()) return refuse("Pause the World before creating a registry");
	if (world->hasAgentBehaviourRegistryReference())
		return refuse("This World already has an Agent behaviour registry");
	if (worldFilepath.empty())
		return refuse("Save the World before creating an Agent behaviour registry");

	error_code error;
	if (!filesystem::is_regular_file(worldFilepath, error) || error)
		return refuse("Save the World before creating an Agent behaviour registry");
	auto const packagePath = core::defaultAgentBehaviourRegistryPackagePath(
		worldFilepath);
	auto const status = filesystem::symlink_status(packagePath, error);
	if ((!error && status.type() != filesystem::file_type::not_found)
		|| (error && error != errc::no_such_file_or_directory))
		return refuse("The adjacent registry package already exists or cannot be inspected");
	if (diagnostic) diagnostic->clear();
	return true;
}

bool canSelectAgentBehaviourRegistry(
	shared_ptr<const core::World> const& world,
	string const& worldFilepath, string* diagnostic)
{
	auto refuse = [diagnostic](string message)
	{
		if (diagnostic) *diagnostic = std::move(message);
		return false;
	};
	if (!world) return refuse("No World is open");
	if (!world->isSimulationPaused()) return refuse("Pause the World before selecting a registry");
	if (worldFilepath.empty())
		return refuse("Save the World before selecting an Agent behaviour registry");

	error_code error;
	if (!filesystem::is_regular_file(worldFilepath, error) || error)
		return refuse("Save the World before selecting an Agent behaviour registry");
	if (diagnostic) diagnostic->clear();
	return true;
}

bool renderBehavioursPanel(shared_ptr<core::World> const& world,
	string const& worldFilepath,
	AgentBehaviourRegistryPathSelector const& selectPackageDirectory)
{
	if (world->hasAgentBehaviourRegistryReference())
	{
		auto const changed = renderAttachedRegistry(
			world, worldFilepath, selectPackageDirectory);
		return renderBehaviourDeleteConfirmation(
			world->getAgentBehaviourRegistry())
			|| renderRegistryChangeConfirmation() || changed;
	}

	ImGui::TextDisabled("No Agent behaviour registry attached.");
	string createDiagnostic;
	auto const canCreate = canCreateAgentBehaviourRegistry(
		world, worldFilepath, &createDiagnostic);
	ImGui::BeginDisabled(!canCreate);
	auto const createClicked = ImGui::Button("Create empty registry");
	ImGui::EndDisabled();
	if (!canCreate && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", createDiagnostic.c_str());

	ImGui::SameLine();
	string selectDiagnostic;
	auto canSelect = canSelectAgentBehaviourRegistry(
		world, worldFilepath, &selectDiagnostic);
	if (!selectPackageDirectory)
	{
		canSelect = false;
		selectDiagnostic = "Registry package selection is unavailable";
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
			auto undo = captureDocumentSnapshot(world);
			if (!undo) throw runtime_error("Could not capture the World before registry creation");
			auto registry = core::createAndAttachAgentBehaviourRegistry(
				*world, worldFilepath);
			commitDocumentEdit(std::move(undo));
			core::addLogMessage("Behaviours", 0, core::LogLevel::Info,
				"Created Agent behaviour registry package "
					+ world->getAgentBehaviourRegistryPackageName()
					+ " (" + registry->getUuid() + ")");
			return true;
		}

		auto selectedPath = selectPackageDirectory();
		if (!selectedPath) return false;
		string diagnostic;
		auto const changed = commitAgentBehaviourRegistrySwitch(world, worldFilepath,
			*selectedPath, diagnostic);
		if (!changed && !diagnostic.empty())
			core::addLogMessage("Behaviours", 0, core::LogLevel::Error, diagnostic);
		return changed;
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Behaviours", 0, core::LogLevel::Error,
			string(createClicked ? "Could not create" : "Could not select")
				+ " Agent behaviour registry: " + error.what());
		return false;
	}
}
