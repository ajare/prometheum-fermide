// Coordinated used Agent behaviour deletion, ticket #162.

#include "BehavioursPanel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/World.h"
#include "imgui/imgui.h"

void runAgentBehaviourDeleteSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path = std::filesystem::temp_directory_path()
			/ ("promethium-fermide-behaviour-delete-"
				+ std::to_string(std::chrono::steady_clock::now()
					.time_since_epoch().count()));
		TemporaryDirectory() { std::filesystem::create_directories(path); }
		~TemporaryDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	void write(std::filesystem::path const& path, std::string const& text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output) throw std::runtime_error("Could not write deletion fixture");
	}

	struct Fixture
	{
		TemporaryDirectory temporary;
		std::filesystem::path package{ temporary.path / "shared.behaviours" };
		std::shared_ptr<core::AgentBehaviourRegistry> registry;
		std::shared_ptr<core::World> first;
		std::shared_ptr<core::World> second;
		core::AgentId firstAgent{};
		core::AgentId secondAgent{};
		core::AgentBehaviourId used{ 1 };
		core::AgentBehaviourId unused{ 2 };

		Fixture()
		{
			std::filesystem::create_directories(package);
			write(package / "simple.lua",
				"return { api_version = 1, factory = function(configuration) return {} end }\n");
			write(package / "behaviours.yaml",
				"version: 1\n"
				"uuid: 123e4567-e89b-42d3-a456-426614174162\n"
				"revision: 1\n"
				"nextBehaviourId: 3\n"
				"behaviours:\n"
				"  - id: 1\n    name: Schedule\n    revision: 1\n    source: simple.lua\n"
				"  - id: 2\n    name: Unused\n    revision: 1\n    source: simple.lua\n");
			auto makeWorld = [&](std::string name, std::string filename,
				core::AgentId& agent)
			{
				auto world = std::make_shared<core::World>(name, 8, 2);
				auto room = world->addRoom("Room", 0, 0, 0, 8, 1);
				world->finishBuild();
				agent = world->createAgent(name + " Agent", room, 0, 1.5f);
				world->pauseSimulation();
				world->saveTo((temporary.path / filename).string());
				return world;
			};
			first = makeWorld("Alpha", "alpha.world.yaml", firstAgent);
			second = makeWorld("Beta", "beta.world.yaml", secondAgent);
			registry = core::selectAndAttachAgentBehaviourRegistry(
				*first, temporary.path / "alpha.world.yaml", package);
			require(core::selectAndAttachAgentBehaviourRegistry(
				*second, temporary.path / "beta.world.yaml", package) == registry,
				"Deletion fixture did not share one registry");
			std::string diagnostic;
			require(first->setAgentBehaviourAssignment(firstAgent, used, 1, {}, &diagnostic)
				&& second->setAgentBehaviourAssignment(secondAgent, used, 1, {}, &diagnostic),
				"Could not assign deletion fixture: " + diagnostic);
			first->markSaved();
			second->markSaved();
			registry->markUnmodified();
			(void)agentBehaviourRegistryDocumentHistory(registry);
			(void)agentBehaviourWorldDocumentHistory(first);
			(void)agentBehaviourWorldDocumentHistory(second);
		}

		~Fixture()
		{
			cancelPendingAgentBehaviourDelete();
			first.reset();
			second.reset();
			forgetAgentBehaviourRegistryDocument(registry);
		}
	};

	void unusedDeletesDirectly()
	{
		Fixture fixture;
		auto& history = agentBehaviourRegistryDocumentHistory(fixture.registry);
		auto const before = history.undoCount();
		requestAgentBehaviourDelete(fixture.registry, fixture.unused);
		require(!agentBehaviourDeletePending()
			&& !fixture.registry->lookupAgentBehaviour(fixture.unused)
			&& history.undoCount() == before + 1,
			"Unused behaviour was not one immediate registry edit");
	}

	void usedDeletionListsCancelsAndCoordinates()
	{
		Fixture fixture;
		// Render the real extracted panel headlessly with the delete controls and
		// modal path present; inspection itself must remain state-free.
		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800, 600);
		unsigned char* pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		ImGui::NewFrame();
		ImGui::Begin("Behaviour deletion smoke");
		require(!renderBehavioursPanel(fixture.first,
			(fixture.temporary.path / "alpha.world.yaml").string()),
			"Rendering deletion controls edited a document");
		ImGui::End();
		ImGui::Render();
		ImGui::DestroyContext();

		auto const text = agentBehaviourDeleteConfirmationText(
			*fixture.registry, fixture.used);
		require(text.find("Alpha") != std::string::npos
			&& text.find("Alpha Agent") != std::string::npos
			&& text.find("Beta") != std::string::npos
			&& text.find("Beta Agent") != std::string::npos,
			"Confirmation did not list every affected World and Agent");
		requestAgentBehaviourDelete(fixture.registry, fixture.used);
		require(agentBehaviourDeletePending(), "Used deletion did not require confirmation");
		cancelPendingAgentBehaviourDelete();
		require(fixture.registry->lookupAgentBehaviour(fixture.used)
			&& fixture.first->getAgentBehaviourAssignment(fixture.firstAgent)
			&& fixture.second->getAgentBehaviourAssignment(fixture.secondAgent),
			"Cancellation changed a participant");

		auto& registryHistory = agentBehaviourRegistryDocumentHistory(fixture.registry);
		auto& firstHistory = agentBehaviourWorldDocumentHistory(fixture.first);
		auto& secondHistory = agentBehaviourWorldDocumentHistory(fixture.second);
		auto const registryUndo = registryHistory.undoCount();
		auto const firstUndo = firstHistory.undoCount();
		auto const secondUndo = secondHistory.undoCount();
		requestAgentBehaviourDelete(fixture.registry, fixture.used);
		std::string diagnostic;
		require(confirmPendingAgentBehaviourDelete(fixture.registry, diagnostic), diagnostic);
		require(!fixture.registry->lookupAgentBehaviour(fixture.used)
			&& !fixture.first->getAgentBehaviourAssignment(fixture.firstAgent)
			&& !fixture.second->getAgentBehaviourAssignment(fixture.secondAgent)
			&& !fixture.first->agentBehaviourOwnsMovement(fixture.firstAgent)
			&& !fixture.second->agentBehaviourOwnsMovement(fixture.secondAgent),
			"Confirmed deletion left a definition, assignment, or movement authority");
		require(registryHistory.undoCount() == registryUndo + 1
			&& firstHistory.undoCount() == firstUndo + 1
			&& secondHistory.undoCount() == secondUndo + 1
			&& fixture.registry->isModified()
			&& fixture.first->isModified() && fixture.second->isModified(),
			"Coordinated deletion did not dirty and history every document independently");
		require(restoreAgentBehaviourRegistrySnapshot(
			fixture.registry, false, &diagnostic)
			&& fixture.registry->lookupAgentBehaviour(fixture.used)
			&& fixture.first->getAgentBehaviourAssignment(fixture.firstAgent)
			&& fixture.second->getAgentBehaviourAssignment(fixture.secondAgent),
			"Coordinated undo did not restore every document: " + diagnostic);
		require(restoreAgentBehaviourRegistrySnapshot(
			fixture.registry, true, &diagnostic)
			&& !fixture.registry->lookupAgentBehaviour(fixture.used)
			&& !fixture.first->getAgentBehaviourAssignment(fixture.firstAgent)
			&& !fixture.second->getAgentBehaviourAssignment(fixture.secondAgent),
			"Coordinated redo did not restore deletion: " + diagnostic);
	}

	void failedParticipantLeavesEverythingUnchanged()
	{
		Fixture fixture;
		require(fixture.second->resumeSimulation(), "Could not run failure participant");
		auto const registryUndo
			= agentBehaviourRegistryDocumentHistory(fixture.registry).undoCount();
		auto const firstUndo
			= agentBehaviourWorldDocumentHistory(fixture.first).undoCount();
		std::string diagnostic;
		require(!commitAgentBehaviourDelete(fixture.registry, fixture.used, diagnostic)
			&& diagnostic.find("Pause") != std::string::npos
			&& fixture.registry->lookupAgentBehaviour(fixture.used)
			&& fixture.first->getAgentBehaviourAssignment(fixture.firstAgent)
			&& fixture.second->getAgentBehaviourAssignment(fixture.secondAgent)
			&& agentBehaviourRegistryDocumentHistory(fixture.registry).undoCount()
				== registryUndo
			&& agentBehaviourWorldDocumentHistory(fixture.first).undoCount()
				== firstUndo,
			"A failed participant partially changed deletion state: " + diagnostic);
	}
}

void runAgentBehaviourDeleteSmokeChecks()
{
	unusedDeletesDirectly();
	usedDeletionListsCancelsAndCoordinates();
	failedParticipantLeavesEverythingUnchanged();
}
