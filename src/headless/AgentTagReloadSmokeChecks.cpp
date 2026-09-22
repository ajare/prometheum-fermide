// Transactional external Agent tag registry reload and lifecycle checks, #143.

#include "TagsPanel.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "DocumentEdit.h"
#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Building.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

void runAgentTagReloadSmokeChecks();

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
				/ ("promethium-fermide-tag-reload-" + purpose + "-"
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

	std::string serializeRegistry(core::AgentTagRegistry const& registry)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		registry.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::string serializeBuilding(core::Building const& building)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		building.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::string readText(std::filesystem::path const& path)
	{
		std::ifstream input(path, std::ios::binary);
		return { std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>() };
	}

	void writeText(std::filesystem::path const& path, std::string const& text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output) throw std::runtime_error("Could not write reload fixture");
	}

	struct SharedFixture
	{
		TemporaryDirectory temporary;
		std::filesystem::path firstPath;
		std::filesystem::path secondPath;
		std::filesystem::path registryPath;
		std::shared_ptr<core::Building> first;
		std::shared_ptr<core::Building> second;
		std::shared_ptr<core::AgentTagRegistry> registry;
		core::AgentTagId tag{};
		core::AgentId firstAgent{};
		core::AgentId secondAgent{};

		explicit SharedFixture(std::string const& purpose)
			: temporary(purpose)
			, firstPath(temporary.path / "first.yaml")
			, secondPath(temporary.path / "second.yaml")
			, registryPath(temporary.path / "first.tags.yaml")
			, first(std::make_shared<core::Building>("First", 8, 2))
			, second(std::make_shared<core::Building>("Second", 8, 2))
		{
			auto const firstCorridor = first->addCorridor(0, 0, 7);
			auto const secondCorridor = second->addCorridor(0, 0, 7);
			first->finishBuild();
			second->finishBuild();
			first->saveTo(firstPath.string());
			second->saveTo(secondPath.string());
			registry = core::createAndAttachAgentTagRegistry(*first, firstPath);
			auto shared = core::selectAndAttachAgentTagRegistry(
				*second, secondPath, registryPath);
			require(shared == registry, "The reload fixture did not share one registry");
			first->pauseSimulation();
			second->pauseSimulation();

			std::string diagnostic;
			tag = commitAgentTagAdd(registry, "workers", diagnostic);
			require(tag && commitAgentTagWalkSpeedModifierAdd(
				registry, tag, diagnostic), diagnostic);
			firstAgent = first->createAgent("Alice", firstCorridor, 0, 2.0f);
			secondAgent = second->createAgent("Bob", secondCorridor, 0, 3.0f);
			require(first->assignAgentTag(firstAgent, tag, &diagnostic)
				&& second->assignAgentTag(secondAgent, tag, &diagnostic), diagnostic);
			require(saveAgentTagRegistry(registry, registryPath.string(), &diagnostic),
				diagnostic);
			first->saveTo(firstPath.string());
			second->saveTo(secondPath.string());
		}

		~SharedFixture()
		{
			forgetAgentTagRegistryDocument(registry);
		}

		void writeExternalRange(float value)
		{
			auto external = core::AgentTagRegistry::loadFrom(registryPath.string());
			std::string diagnostic;
			require(external->setAgentTagWalkSpeedModifier(
				tag, { value, value }, &diagnostic), diagnostic);
			external->saveTo(registryPath.string());
		}
	};

	void externalSaveConflictAndDirtyReloadAreRefused()
	{
		SharedFixture fixture("save-conflict");
		fixture.writeExternalRange(1.2f);
		std::string diagnostic;
		require(commitAgentTagRename(fixture.registry, fixture.tag,
			"local", diagnostic), diagnostic);
		auto const registryBefore = serializeRegistry(*fixture.registry);
		auto const fileBefore = readText(fixture.registryPath);
		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		auto const undoBefore = history.undoCount();

		require(!saveAgentTagRegistry(fixture.registry,
			fixture.registryPath.string(), &diagnostic)
			&& diagnostic.find("changed outside") != std::string::npos,
			"Saving did not detect an external registry revision");
		require(!reloadAgentTagRegistry(fixture.registry,
			fixture.registryPath.string(), &diagnostic)
			&& diagnostic.find("unsaved changes") != std::string::npos,
			"Reload overwrote dirty in-memory registry work");
		require(serializeRegistry(*fixture.registry) == registryBefore
			&& readText(fixture.registryPath) == fileBefore
			&& agentTagRegistryIsModified(fixture.registry)
			&& history.undoCount() == undoBefore,
			"A refused save or dirty reload changed state or history");
	}

	void successfulReloadReconcilesAllBuildingsAndClearsRegistryHistory()
	{
		SharedFixture fixture("success");
		auto& registryHistory = agentTagRegistryDocumentHistory(fixture.registry);
		require(registryHistory.undoCount() > 0 && !registryHistory.isModified(),
			"The successful reload fixture needs saved but non-empty history");
		gBuildingDocumentHistory.clear();
		gBuildingDocumentHistory.commit(
			gBuildingDocumentHistory.capture("unrelated Building history"));
		auto const buildingUndoBefore = gBuildingDocumentHistory.undoCount();

		fixture.writeExternalRange(1.15f);
		std::string diagnostic;
		require(reloadAgentTagRegistry(fixture.registry,
			fixture.registryPath.string(), &diagnostic), diagnostic);
		auto const* definition = fixture.registry
			->getAgentTagWalkSpeedModifier(fixture.tag);
		auto const firstSample = fixture.first->lookupAgent(fixture.firstAgent).entity
			->getWalkSpeedModifierSample();
		auto const secondSample = fixture.second->lookupAgent(fixture.secondAgent).entity
			->getWalkSpeedModifierSample();
		require(definition && definition->range
				== core::AgentModifierRange{ 1.15f, 1.15f }
			&& firstSample && firstSample->propertyRevision == definition->revision
			&& std::abs(firstSample->value - 1.15f) < 0.000001f
			&& secondSample && secondSample->propertyRevision == definition->revision
			&& std::abs(secondSample->value - 1.15f) < 0.000001f,
			"Reload did not update definitions and reconcile every loaded Agent");
		require(fixture.first->isModified() && fixture.second->isModified(),
			"Reload reconciliation did not dirty dependent Buildings");
		require(!agentTagRegistryIsModified(fixture.registry)
			&& registryHistory.undoCount() == 0 && registryHistory.redoCount() == 0,
			"Successful reload did not establish a clean history baseline");
		require(gBuildingDocumentHistory.undoCount() == buildingUndoBefore,
			"Successful registry reload changed Building history");
		gBuildingDocumentHistory.clear();
	}

	void reloadFailuresAreAtomic()
	{
		{
			SharedFixture fixture("running");
			fixture.writeExternalRange(1.1f);
			require(fixture.second->resumeSimulation(),
				"Could not run a dependent Building for the refusal check");
			auto const registryBefore = serializeRegistry(*fixture.registry);
			auto const firstBefore = serializeBuilding(*fixture.first);
			auto const secondBefore = serializeBuilding(*fixture.second);
			auto const historyBefore
				= agentTagRegistryDocumentHistory(fixture.registry).undoCount();
			std::string diagnostic;
			require(!reloadAgentTagRegistry(fixture.registry,
				fixture.registryPath.string(), &diagnostic)
				&& diagnostic.find("Pause Building 'Second'") != std::string::npos,
				"Reload did not require every dependent Building to be paused");
			require(serializeRegistry(*fixture.registry) == registryBefore
				&& serializeBuilding(*fixture.first) == firstBefore
				&& serializeBuilding(*fixture.second) == secondBefore
				&& agentTagRegistryDocumentHistory(fixture.registry).undoCount()
					== historyBefore,
				"A pause refusal changed registry, Building, or history state");
		}
		{
			SharedFixture fixture("malformed");
			auto const registryBefore = serializeRegistry(*fixture.registry);
			auto const firstBefore = serializeBuilding(*fixture.first);
			auto const secondBefore = serializeBuilding(*fixture.second);
			auto const firstModified = fixture.first->isModified();
			auto const secondModified = fixture.second->isModified();
			auto const historyBefore
				= agentTagRegistryDocumentHistory(fixture.registry).undoCount();
			writeText(fixture.registryPath, "agentTagRegistry: [ malformed");
			std::string diagnostic;
			require(!reloadAgentTagRegistry(fixture.registry,
				fixture.registryPath.string(), &diagnostic),
				"Malformed external YAML was accepted");
			require(serializeRegistry(*fixture.registry) == registryBefore
				&& serializeBuilding(*fixture.first) == firstBefore
				&& serializeBuilding(*fixture.second) == secondBefore
				&& fixture.first->isModified() == firstModified
				&& fixture.second->isModified() == secondModified
				&& agentTagRegistryDocumentHistory(fixture.registry).undoCount()
					== historyBefore,
				"Malformed reload changed registry, Buildings, dirtiness, or history");
		}
		{
			SharedFixture fixture("invalid-agent");
			auto external = core::AgentTagRegistry::loadFrom(
				fixture.registryPath.string());
			std::string diagnostic;
			require(external->deleteAgentTag(fixture.tag, &diagnostic), diagnostic);
			external->saveTo(fixture.registryPath.string());
			require(!core::AgentTagRegistry::loadFrom(fixture.registryPath.string())
				->lookupAgentTag(fixture.tag)
				&& fixture.registry->hasLoadedBuilding(fixture.first.get())
				&& fixture.registry->hasLoadedBuilding(fixture.second.get())
				&& fixture.first->getAgentTagAssignmentCount() == 1
				&& fixture.second->getAgentTagAssignmentCount() == 1,
				"The invalid-Agent reload fixture did not retain its intended state");
			auto const registryBefore = serializeRegistry(*fixture.registry);
			auto const firstBefore = serializeBuilding(*fixture.first);
			auto const reloaded = reloadAgentTagRegistry(fixture.registry,
				fixture.registryPath.string(), &diagnostic);
			require(!reloaded && diagnostic.find("Agent '") != std::string::npos,
				"Reload accepted an Agent assignment missing from the replacement registry: "
					+ diagnostic);
			require(serializeRegistry(*fixture.registry) == registryBefore
				&& serializeBuilding(*fixture.first) == firstBefore,
				"Agent validation failure was not transactional");
		}
	}

	void unreferencedRegistryLifetimeFollowsDirtyState()
	{
		TemporaryDirectory temporary("unload");
		auto const buildingPath = temporary.path / "building.yaml";
		auto const registryPath = temporary.path / "building.tags.yaml";
		auto building = std::make_shared<core::Building>("Lifecycle", 6, 2);
		building->addCorridor(0, 0, 5);
		building->finishBuild();
		building->saveTo(buildingPath.string());
		auto registry = core::createAndAttachAgentTagRegistry(*building, buildingPath);
		building->pauseSimulation();
		(void)agentTagRegistryDocumentHistory(registry);

		std::string diagnostic;
		require(commitAgentTagRegistryDetach(building, diagnostic), diagnostic);
		auto reloaded = core::selectAndAttachAgentTagRegistry(
			*building, buildingPath, registryPath);
		require(reloaded != registry,
			"A clean unreferenced registry remained loaded");
		registry = reloaded;
		(void)agentTagRegistryDocumentHistory(registry);

		auto const savedTag = commitAgentTagAdd(registry, "saved-later", diagnostic);
		require(static_cast<bool>(savedTag), diagnostic);
		require(commitAgentTagRegistryDetach(building, diagnostic), diagnostic);
		auto retained = core::selectAndAttachAgentTagRegistry(
			*building, buildingPath, registryPath);
		require(retained == registry && retained->lookupAgentTag(savedTag),
			"A dirty unreferenced registry did not retain unsaved work");
		require(commitAgentTagRegistryDetach(building, diagnostic), diagnostic);
		require(saveAgentTagRegistry(retained, registryPath.string(), &diagnostic),
			diagnostic);
		auto afterSave = core::selectAndAttachAgentTagRegistry(
			*building, buildingPath, registryPath);
		require(afterSave != retained && afterSave->lookupAgentTag(savedTag),
			"Saving an unreferenced registry did not unload it or persist its work");

		auto const discardedTag = commitAgentTagAdd(
			afterSave, "discarded", diagnostic);
		require(static_cast<bool>(discardedTag), diagnostic);
		require(commitAgentTagRegistryDetach(building, diagnostic), diagnostic);
		forgetAgentTagRegistryDocument(afterSave);
		auto afterDiscard = core::selectAndAttachAgentTagRegistry(
			*building, buildingPath, registryPath);
		require(afterDiscard != afterSave
			&& afterDiscard->lookupAgentTag(savedTag)
			&& !afterDiscard->lookupAgentTag(discardedTag),
			"Explicit discard did not release only the unsaved registry revision");
		forgetAgentTagRegistryDocument(afterDiscard);
	}
}

void runAgentTagReloadSmokeChecks()
{
	externalSaveConflictAndDirtyReloadAreRefused();
	successfulReloadReconcilesAllBuildingsAndClearsRegistryHistory();
	reloadFailuresAreAtomic();
	unreferencedRegistryLifetimeFollowsDirtyState();
}
