// Search, loaded-Agent usage, and confirmed Agent tag deletion, ticket #132.

#include "TagsPanel.h"

#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/Building.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct Fixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry{ core::AgentTagRegistry::create() };
		std::shared_ptr<core::Building> first{
			std::make_shared<core::Building>("First Building", 10, 3) };
		std::shared_ptr<core::Building> second{
			std::make_shared<core::Building>("Second Building", 10, 3) };
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
			std::string diagnostic;
			require(first->assignAgentTag(firstAgent, used, &diagnostic)
				&& second->assignAgentTag(secondAgent, used, &diagnostic)
				&& second->assignAgentTag(thirdAgent, used, &diagnostic),
				"The fixture could not assign its Agent tag: " + diagnostic);
			first->markUnmodified();
			second->markUnmodified();
			forgetAgentTagRegistryDocument(registry);
			(void)agentTagRegistryDocumentHistory(registry);
		}

		~Fixture()
		{
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
			"Loaded-Agent usage was not aggregated across dependent Buildings");
		auto const usage = fixture.registry->getLoadedAgentTagUsage(fixture.used);
		require(usage.size() == 2, "The shared registry did not track both loaded Buildings");

		fixture.second.reset();
		require(loadedAgentTagUsageCount(*fixture.registry, fixture.used) == 1,
			"Closing a Building did not remove its Agents from loaded usage");
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
			&& confirmation.find("First Building: 1 Agent") != std::string::npos
			&& confirmation.find("Second Building: 2 Agents") != std::string::npos
			&& confirmation.find("Closed Buildings cannot be counted") != std::string::npos
			&& confirmation.find("stale references") != std::string::npos,
			"The deletion confirmation did not report loaded usage and closed-Building risk");

		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		auto const undoBefore = history.undoCount();
		requestAgentTagDelete(fixture.registry, fixture.used);
		core::AgentTagId pending;
		uint64_t pendingCount{ 0 };
		require(agentTagDeletePending(&pending, &pendingCount)
			&& pending == fixture.used && pendingCount == 3,
			"A used Agent tag did not arm confirmation with its loaded usage");
		require(fixture.registry->lookupAgentTag(fixture.used)
			&& fixture.first->getAgentTags(fixture.firstAgent).contains(fixture.used),
			"Requesting confirmation mutated the registry or an assignment");

		std::string diagnostic;
		require(confirmPendingAgentTagDelete(fixture.registry, diagnostic),
			"Confirmed Agent tag deletion failed: " + diagnostic);
		require(!fixture.registry->lookupAgentTag(fixture.used)
			&& fixture.first->getAgentTags(fixture.firstAgent).empty()
			&& fixture.second->getAgentTags(fixture.secondAgent).empty()
			&& fixture.second->getAgentTags(fixture.thirdAgent).empty(),
			"Confirmed deletion did not remove every loaded Agent tag assignment");
		require(fixture.first->getAgentGroup(fixture.firstAgent) == fixture.firstGroup
			&& fixture.second->getAgentGroup(fixture.secondAgent) == fixture.secondGroup
			&& fixture.second->getAgentGroup(fixture.thirdAgent) == fixture.secondGroup,
			"Agent tag deletion changed Agent group assignments");
		require(history.undoCount() == undoBefore + 1
			&& fixture.first->isModified() && fixture.second->isModified(),
			"The cascade was not one registry edit or did not dirty dependent Buildings");

		require(restoreAgentTagRegistrySnapshot(fixture.registry, false, &diagnostic),
			"Undoing the used Agent tag deletion failed: " + diagnostic);
		require(fixture.registry->lookupAgentTag(fixture.used)
			&& fixture.first->getAgentTags(fixture.firstAgent).contains(fixture.used)
			&& fixture.second->getAgentTags(fixture.secondAgent).contains(fixture.used)
			&& fixture.second->getAgentTags(fixture.thirdAgent).contains(fixture.used),
			"Undo did not restore the definition and all loaded assignments together");
		require(fixture.first->getAgentGroup(fixture.firstAgent) == fixture.firstGroup
			&& fixture.second->getAgentGroup(fixture.secondAgent) == fixture.secondGroup,
			"Undoing Agent tag deletion disturbed Agent groups");
		require(restoreAgentTagRegistrySnapshot(fixture.registry, true, &diagnostic)
			&& !fixture.registry->lookupAgentTag(fixture.used)
			&& fixture.first->getAgentTags(fixture.firstAgent).empty()
			&& fixture.second->getAgentTags(fixture.secondAgent).empty(),
			"Redo did not reapply the complete cascade: " + diagnostic);
	}

	void runningDependentBuildingRefusesWithoutPartialMutation()
	{
		Fixture fixture;
		require(fixture.second->resumeSimulation(),
			"The running-dependency fixture could not resume");
		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		auto const undoBefore = history.undoCount();
		std::string diagnostic;
		require(!commitAgentTagDelete(fixture.registry, fixture.used, diagnostic)
			&& diagnostic.find("Pause") != std::string::npos,
			"A used tag was deleted while one dependent Building was running");
		require(fixture.registry->lookupAgentTag(fixture.used)
			&& fixture.first->getAgentTags(fixture.firstAgent).contains(fixture.used)
			&& fixture.second->getAgentTags(fixture.secondAgent).contains(fixture.used)
			&& history.undoCount() == undoBefore,
			"A refused shared deletion partially mutated state or history");
	}
}

void runAgentTagDeleteSmokeChecks()
{
	filteringAndAggregateLoadedUsage();
	unusedDeletionIsImmediateAndUndoable();
	usedDeletionConfirmsCascadesAndRestoresAtomically();
	runningDependentBuildingRefusesWithoutPartialMutation();
}
