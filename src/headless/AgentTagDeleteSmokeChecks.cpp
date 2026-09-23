// Safe cross-World Agent tag deletion, ticket #141. The checks exercise
// confirmation, property-sample cleanup, exact coordinated undo/redo, atomic
// refusal, and the closed-World stale-ID failure through public workflows.

#include "TagsPanel.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

void runAgentTagDeleteSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeRegistry(core::AgentTagRegistry const& registry)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		registry.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path;

		TemporaryDirectory()
		{
			path = std::filesystem::temp_directory_path()
				/ ("promethium-fermide-tag-delete-"
					+ std::to_string(std::chrono::steady_clock::now()
						.time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}

		~TemporaryDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	struct Fixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry{ core::AgentTagRegistry::create() };
		std::shared_ptr<core::World> first{
			std::make_shared<core::World>("First World", 10, 3) };
		std::shared_ptr<core::World> second{
			std::make_shared<core::World>("Second World", 10, 3) };
		core::AgentTagId used;
		core::AgentTagId unused;
		core::AgentGroupId firstGroup;
		core::AgentGroupId secondGroup;
		core::AgentId firstAgent;
		core::AgentId secondAgent;
		core::AgentId thirdAgent;

		Fixture()
		{
			used = registry->addAgentTag("night-shift");
			unused = registry->addAgentTag("reserve");
			std::string diagnostic;
			require(registry->addAgentTagWalkSpeedModifier(used, &diagnostic)
				&& registry->setAgentTagWalkSpeedModifier(
					used, { 0.9f, 0.9f }, &diagnostic)
				&& registry->addAgentTagHeightModifier(used, &diagnostic)
				&& registry->setAgentTagHeightModifier(
					used, { 0.8f, 0.8f }, &diagnostic),
				"The fixture could not add sampled properties: " + diagnostic);
			registry->markUnmodified();

			first->attachAgentTagRegistry("shared.tags.yaml", registry);
			second->attachAgentTagRegistry("shared.tags.yaml", registry);
			auto const firstCorridor = first->addCorridor(0, 0, 8);
			auto const secondCorridor = second->addCorridor(0, 0, 8);
			first->finishBuild();
			second->finishBuild();
			firstAgent = first->createAgent("Alice", firstCorridor, 0, 1.0f);
			secondAgent = second->createAgent("Bob", secondCorridor, 0, 1.0f);
			thirdAgent = second->createAgent("Cara", secondCorridor, 0, 2.0f);
			firstGroup = first->addAgentGroup("First group");
			secondGroup = second->addAgentGroup("Second group");
			require(first->setAgentGroup(firstAgent, firstGroup)
				&& second->setAgentGroup(secondAgent, secondGroup)
				&& second->setAgentGroup(thirdAgent, secondGroup),
				"The fixture could not assign its Agent groups");
			first->pauseSimulation();
			second->pauseSimulation();
			require(first->assignAgentTag(firstAgent, used, &diagnostic)
				&& second->assignAgentTag(secondAgent, used, &diagnostic)
				&& second->assignAgentTag(thirdAgent, used, &diagnostic),
				"The fixture could not assign its Agent tag: " + diagnostic);
			first->markSaved();
			second->markSaved();
			forgetAgentTagRegistryDocument(registry);
			(void)agentTagRegistryDocumentHistory(registry);
		}

		~Fixture()
		{
			cancelPendingAgentTagDelete();
			forgetAgentTagRegistryDocument(registry);
		}
	};

	void filteringAndAggregateLoadedUsage()
	{
		Fixture fixture;
		require(agentTagNameMatchesFilter("night-shift", "night")
			&& agentTagNameMatchesFilter("night-shift", "#NIGHT")
			&& agentTagNameMatchesFilter("night-shift", "SHIFT")
			&& !agentTagNameMatchesFilter("night-shift", "reserve"),
			"The Tags panel name filter is not case-insensitive or does not include the # display form");
		require(loadedAgentTagUsageCount(*fixture.registry, fixture.used) == 3
			&& loadedAgentTagUsageCount(*fixture.registry, fixture.unused) == 0,
			"Loaded-Agent usage was not aggregated across dependent Worlds");
		auto const usage = fixture.registry->getLoadedAgentTagUsage(fixture.used);
		require(usage.size() == 2, "The shared registry did not track both loaded Worlds");

		auto empty = std::make_shared<core::World>("Empty World", 4, 2);
		empty->attachAgentTagRegistry("shared.tags.yaml", fixture.registry);
		empty->pauseSimulation();
		auto const confirmation = agentTagDeleteConfirmationText(
			*fixture.registry, fixture.used);
		require(confirmation.find("Empty World: 0 Agents") != std::string::npos,
			"The deletion confirmation omitted a loaded World with zero usage");
		empty.reset();

		fixture.second.reset();
		require(loadedAgentTagUsageCount(*fixture.registry, fixture.used) == 1,
			"Closing a World did not remove its Agents from loaded usage");
	}

	void unusedDeletionIsImmediateAndUndoable()
	{
		Fixture fixture;
		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		auto const undoBefore = history.undoCount();
		requestAgentTagDelete(fixture.registry, fixture.unused);
		require(!agentTagDeletePending()
			&& fixture.registry->lookupAgentTag(fixture.unused) == nullptr,
			"Deleting an unused Agent tag did not happen immediately");
		require(history.undoCount() == undoBefore + 1,
			"Deleting an unused Agent tag was not one registry history entry");
		std::string diagnostic;
		require(restoreAgentTagRegistrySnapshot(fixture.registry, false, &diagnostic)
			&& fixture.registry->lookupAgentTag(fixture.unused),
			"Undo did not restore an unused Agent tag: " + diagnostic);
	}

	void usedDeletionConfirmsCascadesAndRestoresAtomically()
	{
		Fixture fixture;
		auto const confirmation = agentTagDeleteConfirmationText(
			*fixture.registry, fixture.used);
		require(confirmation.find("3 loaded Agents") != std::string::npos
			&& confirmation.find("First World: 1 Agent") != std::string::npos
			&& confirmation.find("Second World: 2 Agents") != std::string::npos
			&& confirmation.find("samples sourced from this tag") != std::string::npos
			&& confirmation.find("Closed Worlds cannot be counted") != std::string::npos
			&& confirmation.find("refused when loaded") != std::string::npos,
			"The deletion confirmation did not report usage, samples, and closed-World risk");

		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		auto const undoBefore = history.undoCount();
		auto const registryBefore = serializeRegistry(*fixture.registry);
		auto const firstBefore = serializeWorld(*fixture.first);
		auto const secondBefore = serializeWorld(*fixture.second);
		auto const nextTagBefore = fixture.registry->getNextAgentTagId();
		auto const nextRevisionBefore = fixture.registry->getNextPropertyRevision();
		auto const walkPropertyBefore
			= *fixture.registry->getAgentTagWalkSpeedModifier(fixture.used);
		auto const heightPropertyBefore
			= *fixture.registry->getAgentTagHeightModifier(fixture.used);
		auto const firstWalkBefore = fixture.first->lookupAgent(fixture.firstAgent).entity
			->getWalkSpeedModifierSample();
		auto const firstHeightBefore = fixture.first->lookupAgent(fixture.firstAgent).entity
			->getHeightModifierSample();
		require(firstWalkBefore && firstHeightBefore
			&& fixture.first->getAgentTagSampleCount() == 2
			&& fixture.second->getAgentTagSampleCount() == 4,
			"The deletion fixture did not begin with all expected samples");

		// Requesting and cancelling is state-free, including histories and dirty state.
		requestAgentTagDelete(fixture.registry, fixture.used);
		require(agentTagDeletePending(), "A used Agent tag did not request confirmation");
		cancelPendingAgentTagDelete();
		require(!agentTagDeletePending(),
			"Cancelling Agent tag deletion left confirmation pending");
		require(serializeRegistry(*fixture.registry) == registryBefore,
			"Cancelling Agent tag deletion changed the registry");
		require(serializeWorld(*fixture.first) == firstBefore
			&& serializeWorld(*fixture.second) == secondBefore,
			"Cancelling Agent tag deletion changed a World");
		require(!fixture.registry->isModified(),
			"Cancelling Agent tag deletion changed registry dirty state");
		require(!fixture.first->isModified(),
			"Cancelling Agent tag deletion changed the first World dirty state");
		require(!fixture.second->isModified(),
			"Cancelling Agent tag deletion changed the second World dirty state");
		require(history.undoCount() == undoBefore,
			"Cancelling Agent tag deletion changed registry history");

		requestAgentTagDelete(fixture.registry, fixture.used);
		core::AgentTagId pending;
		uint64_t pendingCount{ 0 };
		require(agentTagDeletePending(&pending, &pendingCount)
			&& pending == fixture.used && pendingCount == 3,
			"A used Agent tag did not arm confirmation with its loaded usage");

		std::string diagnostic;
		require(confirmPendingAgentTagDelete(fixture.registry, diagnostic),
			"Confirmed Agent tag deletion failed: " + diagnostic);
		require(!fixture.registry->lookupAgentTag(fixture.used)
			&& fixture.first->getAgentTags(fixture.firstAgent).empty()
			&& fixture.second->getAgentTags(fixture.secondAgent).empty()
			&& fixture.second->getAgentTags(fixture.thirdAgent).empty()
			&& fixture.first->getAgentTagSampleCount() == 0
			&& fixture.second->getAgentTagSampleCount() == 0,
			"Confirmed deletion did not remove every loaded assignment and sample");
		require(fixture.first->getAgentGroup(fixture.firstAgent) == fixture.firstGroup
			&& fixture.second->getAgentGroup(fixture.secondAgent) == fixture.secondGroup
			&& fixture.second->getAgentGroup(fixture.thirdAgent) == fixture.secondGroup,
			"Agent tag deletion changed Agent group assignments");
		require(history.undoCount() == undoBefore + 1
			&& fixture.first->isModified() && fixture.second->isModified()
			&& fixture.registry->getNextAgentTagId() == nextTagBefore
			&& fixture.registry->getNextPropertyRevision() == nextRevisionBefore,
			"The cascade was not one edit or changed an identity allocator");
		auto const registryAfter = serializeRegistry(*fixture.registry);
		auto const firstAfter = serializeWorld(*fixture.first);
		auto const secondAfter = serializeWorld(*fixture.second);

		require(restoreAgentTagRegistrySnapshot(fixture.registry, false, &diagnostic),
			"Undoing the used Agent tag deletion failed: " + diagnostic);
		require(serializeRegistry(*fixture.registry) == registryBefore
			&& serializeWorld(*fixture.first) == firstBefore
			&& serializeWorld(*fixture.second) == secondBefore
			&& *fixture.registry->getAgentTagWalkSpeedModifier(fixture.used)
				== walkPropertyBefore
			&& *fixture.registry->getAgentTagHeightModifier(fixture.used)
				== heightPropertyBefore
			&& fixture.first->lookupAgent(fixture.firstAgent).entity
				->getWalkSpeedModifierSample() == firstWalkBefore
			&& fixture.first->lookupAgent(fixture.firstAgent).entity
				->getHeightModifierSample() == firstHeightBefore
			&& !fixture.first->isModified() && !fixture.second->isModified(),
			"Undo did not exactly restore tag identity, revisions, assignments, and samples");
		require(fixture.first->getAgentGroup(fixture.firstAgent) == fixture.firstGroup
			&& fixture.second->getAgentGroup(fixture.secondAgent) == fixture.secondGroup,
			"Undoing Agent tag deletion disturbed Agent groups");

		require(restoreAgentTagRegistrySnapshot(fixture.registry, true, &diagnostic)
			&& serializeRegistry(*fixture.registry) == registryAfter
			&& serializeWorld(*fixture.first) == firstAfter
			&& serializeWorld(*fixture.second) == secondAfter
			&& fixture.first->getAgentTagSampleCount() == 0
			&& fixture.second->getAgentTagSampleCount() == 0,
			"Redo did not reapply the exact complete cascade: " + diagnostic);

		auto const replacement = fixture.registry->addAgentTag("replacement");
		require(replacement.value == nextTagBefore && replacement != fixture.used,
			"Deletion or coordinated undo/redo reused the deleted AgentTagId");
	}

	void runningDependentWorldRefusesWithoutPartialMutation()
	{
		Fixture fixture;
		require(fixture.second->resumeSimulation(),
			"The running-dependency fixture could not resume");
		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		auto const undoBefore = history.undoCount();
		auto const registryBefore = serializeRegistry(*fixture.registry);
		auto const firstBefore = serializeWorld(*fixture.first);
		auto const secondBefore = serializeWorld(*fixture.second);
		auto const registryModifiedBefore = fixture.registry->isModified();
		auto const firstModifiedBefore = fixture.first->isModified();
		auto const secondModifiedBefore = fixture.second->isModified();
		std::string diagnostic;
		require(!commitAgentTagDelete(fixture.registry, fixture.used, diagnostic)
			&& diagnostic.find("Pause") != std::string::npos,
			"A used tag was deleted while one dependent World was running");
		require(serializeRegistry(*fixture.registry) == registryBefore
			&& serializeWorld(*fixture.first) == firstBefore
			&& serializeWorld(*fixture.second) == secondBefore
			&& fixture.registry->isModified() == registryModifiedBefore
			&& fixture.first->isModified() == firstModifiedBefore
			&& fixture.second->isModified() == secondModifiedBefore
			&& history.undoCount() == undoBefore,
			"A refused shared deletion partially mutated documents, dirty state, or history");
	}

	void closedWorldRetainingDeletedIdIsRefused()
	{
		TemporaryDirectory temporary;
		auto const closedPath = temporary.path / "closed.world.yaml";
		auto const editorPath = temporary.path / "editor.world.yaml";
		auto const registryPath = temporary.path / "closed.tags.yaml";

		auto closed = std::make_shared<core::World>("Closed World", 8, 2);
		auto const closedCorridor = closed->addCorridor(0, 0, 7);
		closed->finishBuild();
		closed->saveTo(closedPath.string());
		auto registry = core::createAndAttachAgentTagRegistry(*closed, closedPath);
		closed->pauseSimulation();
		auto const tag = registry->addAgentTag("shared");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(tag, &diagnostic), diagnostic);
		auto const closedAgent = closed->createAgent(
			"Closed Agent", closedCorridor, 0, 2.0f);
		require(closed->assignAgentTag(closedAgent, tag, &diagnostic), diagnostic);
		registry->saveTo(registryPath.string());
		closed->saveTo(closedPath.string());

		auto editor = std::make_shared<core::World>("Loaded Editor", 8, 2);
		auto const editorCorridor = editor->addCorridor(0, 0, 7);
		editor->finishBuild();
		editor->saveTo(editorPath.string());
		auto shared = core::selectAndAttachAgentTagRegistry(
			*editor, editorPath, registryPath);
		require(shared == registry, "The closed-World fixture did not share its registry");
		editor->pauseSimulation();
		auto const editorAgent = editor->createAgent(
			"Loaded Agent", editorCorridor, 0, 2.0f);
		require(editor->assignAgentTag(editorAgent, tag, &diagnostic), diagnostic);
		closed.reset();

		forgetAgentTagRegistryDocument(registry);
		(void)agentTagRegistryDocumentHistory(registry);
		requestAgentTagDelete(registry, tag);
		require(agentTagDeletePending(nullptr, nullptr)
			&& confirmPendingAgentTagDelete(registry, diagnostic),
			"The confirmed deletion with a closed dependant failed: " + diagnostic);
		require(editor->getAgentTags(editorAgent).empty()
			&& editor->getAgentTagSampleCount() == 0,
			"Deletion did not clear the loaded dependant before testing the closed one");
		registry->saveTo(registryPath.string());

		std::string refusal;
		try
		{
			(void)core::loadWorldDocument(closedPath);
		}
		catch (std::exception const& error)
		{
			refusal = error.what();
		}
		require(refusal.find("Closed Agent") != std::string::npos
			&& refusal.find(std::to_string(tag.value)) != std::string::npos
			&& !registry->lookupAgentTag(tag)
			&& editor->getAgentTags(editorAgent).empty(),
			"A closed World retaining the deleted AgentTagId was not refused: " + refusal);
		forgetAgentTagRegistryDocument(registry);
	}
}

void runAgentTagDeleteSmokeChecks()
{
	filteringAndAggregateLoadedUsage();
	unusedDeletionIsImmediateAndUndoable();
	usedDeletionConfirmsCascadesAndRestoresAtomically();
	runningDependentWorldRefusesWithoutPartialMutation();
	closedWorldRetainingDeletedIdIsRefused();
}
