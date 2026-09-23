// Coordinated shared-registry definition edits, ticket #137. This is the
// dedicated two-World headless scenario: one edit updates both dependent
// Worlds, and registry undo/redo restores the complete shared transaction.

#include "TagsPanel.h"

#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/World.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

void runAgentTagCoordinationSmokeChecks();

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

	struct SharedFixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry{
			core::AgentTagRegistry::create() };
		std::shared_ptr<core::World> first{
			std::make_shared<core::World>("First coordinated World", 8, 2) };
		std::shared_ptr<core::World> second{
			std::make_shared<core::World>("Second coordinated World", 8, 2) };
		core::AgentTagId tag{};
		core::AgentId firstAgent{};
		core::AgentId secondAgent{};

		SharedFixture()
		{
			std::string diagnostic;
			tag = registry->addAgentTag("shared");
			require(registry->addAgentTagWalkSpeedModifier(tag, &diagnostic), diagnostic);
			require(registry->setAgentTagWalkSpeedModifier(
				tag, { 0.9f, 0.9f }, &diagnostic), diagnostic);

			first->attachAgentTagRegistry("shared.tags.yaml", registry);
			second->attachAgentTagRegistry("shared.tags.yaml", registry);
			auto const firstCorridor = first->addCorridor(0, 0, 7);
			auto const secondCorridor = second->addCorridor(0, 0, 7);
			first->finishBuild();
			second->finishBuild();
			firstAgent = first->createAgent("First Agent", firstCorridor, 0, 1.5f);
			secondAgent = second->createAgent("Second Agent", secondCorridor, 0, 2.5f);
			first->pauseSimulation();
			second->pauseSimulation();
			require(first->assignAgentTag(firstAgent, tag, &diagnostic), diagnostic);
			require(second->assignAgentTag(secondAgent, tag, &diagnostic), diagnostic);
			markClean();
		}

		~SharedFixture()
		{
			forgetAgentTagRegistryDocument(registry);
		}

		void markClean()
		{
			registry->markUnmodified();
			first->markSaved();
			if (second) second->markSaved();
			forgetAgentTagRegistryDocument(registry);
			(void)agentTagRegistryDocumentHistory(registry);
		}

		core::AgentPropertySample firstSample() const
		{
			auto const& sample = first->lookupAgent(firstAgent).entity
				->getWalkSpeedModifierSample();
			require(sample.has_value(), "The first coordinated Agent has no sample");
			return *sample;
		}

		core::AgentPropertySample secondSample() const
		{
			auto const& sample = second->lookupAgent(secondAgent).entity
				->getWalkSpeedModifierSample();
			require(sample.has_value(), "The second coordinated Agent has no sample");
			return *sample;
		}
	};

	void oneEditUpdatesAndRestoresTwoWorlds()
	{
		SharedFixture fixture;
		std::string diagnostic;
		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		auto const oldProperty
			= *fixture.registry->getAgentTagWalkSpeedModifier(fixture.tag);
		auto const oldFirstSample = fixture.firstSample();
		auto const oldSecondSample = fixture.secondSample();

		require(commitAgentTagWalkSpeedModifierEdit(fixture.registry, fixture.tag,
			{ 1.2f, 1.2f }, diagnostic), diagnostic);
		auto const newProperty
			= *fixture.registry->getAgentTagWalkSpeedModifier(fixture.tag);
		auto const newFirstSample = fixture.firstSample();
		auto const newSecondSample = fixture.secondSample();
		require(newProperty.revision == oldProperty.revision + 1
			&& newFirstSample.propertyRevision == newProperty.revision
			&& newSecondSample.propertyRevision == newProperty.revision
			&& std::abs(newFirstSample.value - 1.2f) < 0.000001f
			&& std::abs(newSecondSample.value - 1.2f) < 0.000001f
			&& fixture.first->isModified() && fixture.second->isModified()
			&& history.undoCount() == 1,
			"One registry edit did not update and dirty both loaded Worlds");

		require(restoreAgentTagRegistrySnapshot(fixture.registry, false, &diagnostic),
			diagnostic);
		require(*fixture.registry->getAgentTagWalkSpeedModifier(fixture.tag)
				== oldProperty
			&& fixture.firstSample() == oldFirstSample
			&& fixture.secondSample() == oldSecondSample
			&& fixture.first->getAgentTags(fixture.firstAgent).contains(fixture.tag)
			&& fixture.second->getAgentTags(fixture.secondAgent).contains(fixture.tag)
			&& !fixture.first->isModified() && !fixture.second->isModified(),
			"Registry undo did not restore both Worlds' exact old state");
		require(restoreAgentTagRegistrySnapshot(fixture.registry, true, &diagnostic),
			diagnostic);
		require(*fixture.registry->getAgentTagWalkSpeedModifier(fixture.tag)
				== newProperty
			&& fixture.firstSample() == newFirstSample
			&& fixture.secondSample() == newSecondSample
			&& fixture.first->isModified() && fixture.second->isModified(),
			"Registry redo rerolled or omitted one World's replacement sample");

		// Deletion shares the same transaction boundary and restores assignments,
		// definitions, revisions, and the exact samples in both Worlds.
		fixture.markClean();
		auto const registryBeforeDelete = serializeRegistry(*fixture.registry);
		auto const firstBeforeDelete = serializeWorld(*fixture.first);
		auto const secondBeforeDelete = serializeWorld(*fixture.second);
		require(commitAgentTagDelete(fixture.registry, fixture.tag, diagnostic), diagnostic);
		require(!fixture.registry->lookupAgentTag(fixture.tag)
			&& fixture.first->getAgentTags(fixture.firstAgent).empty()
			&& fixture.second->getAgentTags(fixture.secondAgent).empty()
			&& fixture.first->isModified() && fixture.second->isModified(),
			"Coordinated deletion left a definition, assignment, or clean World");
		require(restoreAgentTagRegistrySnapshot(fixture.registry, false, &diagnostic),
			diagnostic);
		require(serializeRegistry(*fixture.registry) == registryBeforeDelete
			&& serializeWorld(*fixture.first) == firstBeforeDelete
			&& serializeWorld(*fixture.second) == secondBeforeDelete
			&& !fixture.first->isModified() && !fixture.second->isModified(),
			"Deletion undo did not exactly restore definitions, revisions, assignments, and samples");
		require(restoreAgentTagRegistrySnapshot(fixture.registry, true, &diagnostic),
			diagnostic);
		require(!fixture.registry->lookupAgentTag(fixture.tag)
			&& fixture.first->getAgentTags(fixture.firstAgent).empty()
			&& fixture.second->getAgentTags(fixture.secondAgent).empty(),
			"Deletion redo did not restore the complete coordinated result");
	}

	void runningDependencyDisablesEveryDefinitionEdit()
	{
		SharedFixture fixture;
		require(fixture.second->resumeSimulation(),
			"The running dependent World could not resume");
		std::string diagnostic;
		require(!fixture.registry->definitionEditsAreAllowed(&diagnostic)
			&& diagnostic.find("Second coordinated World") != std::string::npos,
			"A running dependent World did not disable registry definition edits");
		auto const registryBefore = serializeRegistry(*fixture.registry);
		auto const firstBefore = serializeWorld(*fixture.first);
		auto const secondBefore = serializeWorld(*fixture.second);
		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		require(!commitAgentTagWalkSpeedModifierEdit(fixture.registry, fixture.tag,
			{ 1.0f, 1.0f }, diagnostic)
			&& diagnostic.find("Pause World") != std::string::npos
			&& serializeRegistry(*fixture.registry) == registryBefore
			&& serializeWorld(*fixture.first) == firstBefore
			&& serializeWorld(*fixture.second) == secondBefore
			&& history.undoCount() == 0,
			"A running dependency allowed or partially applied a definition edit");
	}

	void crossWorldConflictIsRejectedBeforeMutation()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const source = registry->addAgentTag("source");
		auto const pending = registry->addAgentTag("pending");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(source, &diagnostic), diagnostic);
		auto first = std::make_shared<core::World>("Conflict first", 6, 2);
		auto second = std::make_shared<core::World>("Conflict second", 6, 2);
		first->attachAgentTagRegistry("conflict.tags.yaml", registry);
		second->attachAgentTagRegistry("conflict.tags.yaml", registry);
		auto const firstCorridor = first->addCorridor(0, 0, 5);
		auto const secondCorridor = second->addCorridor(0, 0, 5);
		first->finishBuild();
		second->finishBuild();
		auto const firstAgent = first->createAgent("Compatible", firstCorridor);
		auto const secondAgent = second->createAgent("Conflicting", secondCorridor);
		first->pauseSimulation();
		second->pauseSimulation();
		require(first->assignAgentTag(firstAgent, pending, &diagnostic), diagnostic);
		require(second->assignAgentTag(secondAgent, source, &diagnostic)
			&& second->assignAgentTag(secondAgent, pending, &diagnostic), diagnostic);
		registry->markUnmodified();
		first->markSaved();
		second->markSaved();
		forgetAgentTagRegistryDocument(registry);
		auto& history = agentTagRegistryDocumentHistory(registry);
		auto const registryBefore = serializeRegistry(*registry);
		auto const firstBefore = serializeWorld(*first);
		auto const secondBefore = serializeWorld(*second);
		auto const revisionBefore = registry->getNextPropertyRevision();

		require(!commitAgentTagWalkSpeedModifierAdd(registry, pending, diagnostic)
			&& diagnostic.find("Conflicting") != std::string::npos
			&& diagnostic.find("#source") != std::string::npos
			&& registry->getNextPropertyRevision() == revisionBefore
			&& serializeRegistry(*registry) == registryBefore
			&& serializeWorld(*first) == firstBefore
			&& serializeWorld(*second) == secondBefore
			&& !registry->isModified() && !first->isModified() && !second->isModified()
			&& history.undoCount() == 0,
			"A conflict in the second World partially changed coordinated state");
		forgetAgentTagRegistryDocument(registry);
	}

	void closingParticipantInvalidatesIncompleteHistory()
	{
		SharedFixture fixture;
		std::string diagnostic;
		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		require(commitAgentTagWalkSpeedModifierEdit(fixture.registry, fixture.tag,
			{ 1.1f, 1.1f }, diagnostic), diagnostic);
		require(history.canUndo(), "The coordinated edit created no undo entry");
		fixture.second.reset();
		require(!history.canUndo() && history.undoCount() == 0
			&& !restoreAgentTagRegistrySnapshot(
				fixture.registry, false, &diagnostic),
			"Closing a participating World retained an incomplete registry history entry");
	}
}

void runAgentTagCoordinationSmokeChecks()
{
	oneEditUpdatesAndRestoresTwoWorlds();
	runningDependencyDisablesEveryDefinitionEdit();
	crossWorldConflictIsRejectedBeforeMutation();
	closingParticipantInvalidatesIncompleteHistory();
}
