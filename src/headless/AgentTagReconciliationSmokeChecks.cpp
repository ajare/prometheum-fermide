// Closed-Building Agent tag registry reconciliation, ticket #138.

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Building.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

void runAgentTagReconciliationSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path;

		explicit TemporaryDirectory(std::string const& purpose)
		{
			path = std::filesystem::temp_directory_path()
				/ ("promethium-fermide-tag-reconciliation-" + purpose + "-"
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

	std::string serializeBuilding(core::Building const& building)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		building.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
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

	std::string readText(std::filesystem::path const& path)
	{
		std::ifstream input(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	void writeText(std::filesystem::path const& path, std::string const& text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output) throw std::runtime_error("Could not write reconciliation fixture");
	}

	void validClosedBuildingEvolutionIsReconciled()
	{
		TemporaryDirectory temporary("valid");
		auto const buildingPath = temporary.path / "station.yaml";
		auto const evolverPath = temporary.path / "evolver.yaml";
		auto source = std::make_shared<core::Building>("Closed station", 10, 3);
		auto const corridor = source->addCorridor(0, 0, 9);
		source->finishBuild();
		source->saveTo(buildingPath.string());
		auto registry = core::createAndAttachAgentTagRegistry(*source, buildingPath);
		source->pauseSimulation();

		auto const stale = registry->addAgentTag("stale");
		auto const newlyModified = registry->addAgentTag("new-modifier");
		auto const obsolete = registry->addAgentTag("obsolete");
		auto const unrelated = registry->addAgentTag("unrelated");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(stale, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(
			stale, { 0.9f, 0.9f }, &diagnostic), diagnostic);
		require(registry->addAgentTagHeightModifier(obsolete, &diagnostic), diagnostic);
		require(registry->setAgentTagHeightModifier(
			obsolete, { 0.8f, 0.8f }, &diagnostic), diagnostic);
		require(registry->addAgentTagWalkSpeedModifier(unrelated, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(
			unrelated, { 0.8f, 1.2f }, &diagnostic), diagnostic);

		auto const alice = source->createAgent("Alice", corridor, 0, 1.5f);
		auto const bob = source->createAgent("Bob", corridor, 0, 3.5f);
		auto const carol = source->createAgent("Carol", corridor, 0, 5.5f);
		require(source->assignAgentTag(alice, stale, &diagnostic)
			&& source->assignAgentTag(alice, newlyModified, &diagnostic)
			&& source->assignAgentTag(bob, obsolete, &diagnostic)
			&& source->assignAgentTag(carol, unrelated, &diagnostic), diagnostic);
		auto const oldAliceWalk = *source->lookupAgent(alice).entity
			->getWalkSpeedModifierSample();
		auto const oldBobHeight = *source->lookupAgent(bob).entity
			->getHeightModifierSample();
		auto const unchangedCarolWalk = *source->lookupAgent(carol).entity
			->getWalkSpeedModifierSample();
		registry->saveTo((temporary.path / "station.tags.yaml").string());
		source->saveTo(buildingPath.string());

		// Keep the shared registry open through another Building while the tagged
		// Building is closed, matching the user workflow in the ticket.
		auto evolver = std::make_shared<core::Building>("Registry editor", 4, 2);
		evolver->saveTo(evolverPath.string());
		auto shared = core::selectAndAttachAgentTagRegistry(
			*evolver, evolverPath, temporary.path / "station.tags.yaml");
		require(shared == registry, "The evolution fixture did not share one registry instance");
		evolver->pauseSimulation();
		source.reset();

		require(registry->setAgentTagWalkSpeedModifier(
			stale, { 1.1f, 1.1f }, &diagnostic), diagnostic);
		require(registry->addAgentTagHeightModifier(newlyModified, &diagnostic), diagnostic);
		require(registry->setAgentTagHeightModifier(
			newlyModified, { 0.75f, 0.75f }, &diagnostic), diagnostic);
		require(registry->removeAgentTagHeightModifier(obsolete, &diagnostic), diagnostic);
		require(registry->renameAgentTag(unrelated, "renamed", &diagnostic), diagnostic);
		require(registry->addAgentTagColour(unrelated, &diagnostic), diagnostic);
		require(registry->setAgentTagColour(
			unrelated, { 12, 34, 56 }, &diagnostic), diagnostic);
		registry->saveTo((temporary.path / "station.tags.yaml").string());

		auto reopened = core::loadBuildingDocument(buildingPath);
		require(reopened->isModified(),
			"Repairing closed-Building samples did not mark the Building modified");
		auto const* reopenedAlice = reopened->lookupAgent(alice).entity;
		auto const* reopenedBob = reopened->lookupAgent(bob).entity;
		auto const* reopenedCarol = reopened->lookupAgent(carol).entity;
		auto const aliceWalk = reopenedAlice->getWalkSpeedModifierSample();
		auto const aliceHeight = reopenedAlice->getHeightModifierSample();
		require(aliceWalk && aliceWalk->sourceTag == stale
			&& aliceWalk->propertyRevision
				== registry->getAgentTagWalkSpeedModifier(stale)->revision
			&& std::abs(aliceWalk->value - 1.1f) < 0.000001f
			&& *aliceWalk != oldAliceWalk,
			"A stale Walk speed revision was not resampled against the evolved registry");
		require(aliceHeight && aliceHeight->sourceTag == newlyModified
			&& aliceHeight->propertyRevision
				== registry->getAgentTagHeightModifier(newlyModified)->revision
			&& std::abs(aliceHeight->value - 0.75f) < 0.000001f,
			"A newly added Height modifier did not produce its missing sample");
		require(!reopenedBob->getHeightModifierSample()
			&& oldBobHeight.sourceTag == obsolete,
			"A sample for a removed property was not discarded");
		require(reopenedCarol->getWalkSpeedModifierSample()
			== std::optional<core::AgentPropertySample>{ unchangedCarolWalk }
			&& registry->getAgentTagName(unrelated) == "renamed",
			"A tag rename or Colour-only edit resampled an unrelated modifier");

		// Once the repaired Building is saved, another open is stable and clean.
		auto const repairedAliceWalk = *aliceWalk;
		auto const repairedAliceHeight = *aliceHeight;
		reopened->saveTo(buildingPath.string());
		reopened.reset();
		auto stable = core::loadBuildingDocument(buildingPath);
		require(!stable->isModified()
			&& stable->lookupAgent(alice).entity->getWalkSpeedModifierSample()
				== std::optional<core::AgentPropertySample>{ repairedAliceWalk }
			&& stable->lookupAgent(alice).entity->getHeightModifierSample()
				== std::optional<core::AgentPropertySample>{ repairedAliceHeight },
			"A saved reconciliation rerolled or dirtied an unchanged reopen");
	}

	struct RefusalFixture
	{
		TemporaryDirectory temporary;
		std::filesystem::path buildingPath;
		std::filesystem::path registryPath;
		std::shared_ptr<core::Building> current;
		std::shared_ptr<core::AgentTagRegistry> registry;
		core::AgentTagId primary{};
		core::AgentTagId secondary{};

		explicit RefusalFixture(std::string const& purpose)
			: temporary(purpose)
			, buildingPath(temporary.path / "closed.yaml")
			, registryPath(temporary.path / "closed.tags.yaml")
		{
			auto closed = std::make_shared<core::Building>("Closed invalid", 8, 2);
			auto const corridor = closed->addCorridor(0, 0, 7);
			closed->finishBuild();
			closed->saveTo(buildingPath.string());
			registry = core::createAndAttachAgentTagRegistry(*closed, buildingPath);
			closed->pauseSimulation();
			primary = registry->addAgentTag("primary");
			secondary = registry->addAgentTag("secondary");
			std::string diagnostic;
			require(registry->addAgentTagWalkSpeedModifier(primary, &diagnostic), diagnostic);
			require(registry->setAgentTagWalkSpeedModifier(
				primary, { 0.9f, 0.9f }, &diagnostic), diagnostic);
			auto const alice = closed->createAgent("Alice", corridor, 0, 2.5f);
			require(closed->assignAgentTag(alice, primary, &diagnostic)
				&& closed->assignAgentTag(alice, secondary, &diagnostic), diagnostic);
			registry->saveTo(registryPath.string());
			closed->saveTo(buildingPath.string());

			// This Building is the pre-existing registry-manager participant whose
			// state every refused open must preserve.
			current = std::make_shared<core::Building>("Current work", 5, 2);
			auto const currentPath = temporary.path / "current.yaml";
			current->saveTo(currentPath.string());
			auto shared = core::selectAndAttachAgentTagRegistry(
				*current, currentPath, registryPath);
			require(shared == registry, "The refusal fixture did not share its registry");
			current->pauseSimulation();
			current->markSaved();
			closed.reset();
		}

		YAML::Node buildingYaml() const
		{
			return YAML::Load(readText(buildingPath));
		}

		void writeBuilding(YAML::Node const& document)
		{
			writeText(buildingPath, YAML::Dump(document));
		}

		void expectRefusal(std::vector<std::string> const& diagnosticParts)
		{
			auto const* identity = current.get();
			auto const buildingBefore = serializeBuilding(*current);
			auto const registryBefore = serializeRegistry(*registry);
			auto const registryModifiedBefore = registry->isModified();
			auto const buildingModifiedBefore = current->isModified();
			auto const usageBefore = registry->getLoadedAgentTagUsage(secondary);
			require(usageBefore.size() == 1 && usageBefore.front().building == identity,
				"The refusal fixture began with unexpected registry-manager state");

			std::string diagnostic;
			try
			{
				current = core::loadBuildingDocument(buildingPath);
			}
			catch (std::exception const& error)
			{
				diagnostic = error.what();
			}
			require(!diagnostic.empty(), "Corrupted current Agent tag data was accepted");
			for (auto const& part : diagnosticParts)
			{
				require(diagnostic.find(part) != std::string::npos,
					"Load refusal did not identify '" + part + "': " + diagnostic);
			}
			auto const usageAfter = registry->getLoadedAgentTagUsage(secondary);
			require(current.get() == identity
				&& serializeBuilding(*current) == buildingBefore
				&& serializeRegistry(*registry) == registryBefore
				&& current->isModified() == buildingModifiedBefore
				&& registry->isModified() == registryModifiedBefore
				&& usageAfter.size() == 1 && usageAfter.front().building == identity,
				"A refused open changed the prior Building or registry-manager state");
		}
	};

	void unknownAndDeletedTagsAreRefused()
	{
		{
			RefusalFixture fixture("unknown");
			auto document = fixture.buildingYaml();
			document["agents"][0]["agent"]["tags"].push_back(9999);
			fixture.writeBuilding(document);
			fixture.expectRefusal({ "Alice", "9999" });
		}
		{
			RefusalFixture fixture("deleted");
			std::string diagnostic;
			require(fixture.registry->deleteAgentTag(fixture.primary, &diagnostic), diagnostic);
			fixture.registry->saveTo(fixture.registryPath.string());
			fixture.expectRefusal({ "Alice", std::to_string(fixture.primary.value) });
		}
	}

	void inheritedConflictAndWrongSourceAreRefused()
	{
		{
			RefusalFixture fixture("conflict");
			std::string diagnostic;
			require(fixture.registry->addAgentTagWalkSpeedModifier(
				fixture.secondary, &diagnostic), diagnostic);
			fixture.registry->saveTo(fixture.registryPath.string());
			fixture.expectRefusal({ "Alice", "Walk speed modifier", "#primary", "#secondary" });
		}
		{
			RefusalFixture fixture("wrong-source");
			auto document = fixture.buildingYaml();
			document["agents"][0]["agent"]["propertySamples"][0]["sourceTag"]
				= fixture.secondary.value;
			fixture.writeBuilding(document);
			fixture.expectRefusal({ "Alice", "Walk speed modifier", "#secondary", "#primary" });
		}
	}

	void invalidCurrentRevisionValuesAreRefused()
	{
		{
			RefusalFixture fixture("non-finite");
			auto document = fixture.buildingYaml();
			document["agents"][0]["agent"]["propertySamples"][0]["value"]
				= std::numeric_limits<float>::quiet_NaN();
			fixture.writeBuilding(document);
			fixture.expectRefusal({ "Alice", "non-finite", "Walk speed modifier", "#primary" });
		}
		{
			RefusalFixture fixture("out-of-range");
			auto document = fixture.buildingYaml();
			document["agents"][0]["agent"]["propertySamples"][0]["value"] = 1.2f;
			fixture.writeBuilding(document);
			fixture.expectRefusal({ "Alice", "current-revision", "Walk speed modifier",
				"#primary", "outside" });
		}
	}
}

void runAgentTagReconciliationSmokeChecks()
{
	validClosedBuildingEvolutionIsReconciled();
	unknownAndDeletedTagsAreRefused();
	inheritedConflictAndWrongSourceAreRefused();
	invalidCurrentRevisionValuesAreRefused();
}
