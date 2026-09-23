// External Agent behaviour registry package workflow and atomic reload checks.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include "core/AgentTagRegistryDocument.h"
#include <stdexcept>
#include <string>

#include "BehavioursPanel.h"
#include "DocumentEdit.h"
#include "imgui/imgui.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/Building.h"
#include "core/Log.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path;

		TemporaryDirectory()
		{
			path = std::filesystem::temp_directory_path()
				/ ("prometheium-fermide-behaviours-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}

		~TemporaryDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	std::string readText(std::filesystem::path const& path)
	{
		std::ifstream input(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	void writeText(std::filesystem::path const& path, std::string const& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output) throw std::runtime_error("Could not write behaviour package fixture");
	}

	std::string serializeBuilding(core::Building const& building)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		building.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::Building> loadBuilding(std::filesystem::path const& path)
	{
		auto loaded = std::make_shared<core::Building>("Loading", 1, 1);
		loaded->pauseSimulation();
		auto reader = core::YamlSerializer::fromFile(path.string());
		reader->deserialize();
		core::SerializationWorkData workData;
		require(loaded->deserialize(*reader, workData), "The Building did not reload");
		return loaded;
	}

	std::filesystem::path manifestPath(std::filesystem::path const& packageDirectory)
	{
		return core::agentBehaviourRegistryManifestPath(packageDirectory);
	}

	std::string emptyManifestYaml(std::string const& uuid)
	{
		return ""
			"  version: 1\n"
			"  uuid: " + uuid + "\n"
			"  nextBehaviourId: 1\n"
			"  behaviours: []\n";
	}

	std::vector<core::AgentBehaviourSchemaField> scheduleSchema()
	{
		std::vector<core::AgentBehaviourSchemaField> schema;
		schema.push_back({ "enabled", core::AgentBehaviourSchemaType::Boolean, {} });
		schema.push_back({ "dwellTicks", core::AgentBehaviourSchemaType::Duration, {} });
		schema.push_back({ "stops", core::AgentBehaviourSchemaType::List,
			{ { "stop", core::AgentBehaviourSchemaType::Record,
				{ { "at", core::AgentBehaviourSchemaType::Marker, {} },
					{ "forTicks", core::AgentBehaviourSchemaType::Duration, {} } } } },
			false, core::AgentBehaviourConfigurationList{} });
		return schema;
	}

	void savedBuildingCreatesAndReopensAdjacentPackage()
	{
		TemporaryDirectory temporary;
		auto building = std::make_shared<core::Building>("Station", 4, 2);
		building->pauseSimulation();
		auto const buildingPath = temporary.path / "station.yaml";

		bool unsavedRefused{ false };
		try
		{
			(void)core::createAndAttachAgentBehaviourRegistry(*building, buildingPath);
		}
		catch (std::exception const& error)
		{
			unsavedRefused = std::string(error.what()).find("Save")
				!= std::string::npos;
		}
		require(unsavedRefused && !building->hasAgentBehaviourRegistryReference(),
			"Registry creation was enabled before the Building was saved");

		building->saveTo(buildingPath.string());
		auto registry = core::createAndAttachAgentBehaviourRegistry(*building, buildingPath);
		auto const packageDirectory = temporary.path / "station.behaviours";
		require(std::filesystem::is_directory(packageDirectory),
			"The adjacent .behaviours package directory was not created");
		require(std::filesystem::is_regular_file(manifestPath(packageDirectory)),
			"The package manifest was not created");
		require(building->hasAgentBehaviourRegistryReference()
			&& building->hasAttachedAgentBehaviourRegistry(),
			"The new registry was not attached to the Building");
		require(building->getAgentBehaviourRegistryPackageName() == "station.behaviours",
			"The Building did not retain a package-directory basename reference");
		require(building->getExpectedAgentBehaviourRegistryUuid() == registry->getUuid()
			&& core::AgentBehaviourRegistry::uuidIsValid(registry->getUuid()),
			"The Building did not retain the registry's stable UUID");
		require(registry->getNextBehaviourId() == 1 && registry->getBehaviourCount() == 0,
			"A new registry did not start its allocator at one with no behaviours");

		auto const manifestYaml = readText(manifestPath(packageDirectory));
		require(manifestYaml.find("version: 1") != std::string::npos
			&& manifestYaml.find("uuid: " + registry->getUuid()) != std::string::npos
			&& manifestYaml.find("nextBehaviourId: 1") != std::string::npos
			&& manifestYaml.find("behaviours:") != std::string::npos,
			"The empty manifest omitted its schema, UUID, allocator, or behaviour collection");

		// Persist the attachment, then model closing both documents and reopening
		// through the same core workflow used by the GUI.
		building->saveTo(buildingPath.string());
		auto const buildingYaml = readText(buildingPath);
		require(buildingYaml.find("version: 14") != std::string::npos
			&& buildingYaml.find("package: station.behaviours") != std::string::npos
			&& buildingYaml.find("expectedUuid: " + registry->getUuid()) != std::string::npos,
			"The Building did not persist its version-12 registry reference");

		auto reopened = core::loadBuildingDocument(buildingPath);
		require(reopened->hasAgentBehaviourRegistryReference()
			&& reopened->hasAttachedAgentBehaviourRegistry(),
			"The referenced empty registry did not survive close and reopen");
		require(reopened->getAgentBehaviourRegistry() == registry,
			"Reopen did not share the canonical package's loaded registry instance");

		// Replacing the manifest must not silently reinterpret the reference.
		auto replacement = core::AgentBehaviourRegistry::create();
		replacement->saveTo(manifestPath(packageDirectory).string());
		auto substituted = loadBuilding(buildingPath);
		bool mismatchRefused{ false };
		try
		{
			(void)core::loadAndAttachAgentBehaviourRegistry(*substituted, buildingPath);
		}
		catch (std::exception const& error)
		{
			mismatchRefused = std::string(error.what()).find("UUID mismatch")
				!= std::string::npos;
		}
		require(mismatchRefused && !substituted->hasAttachedAgentBehaviourRegistry(),
			"A substituted registry with a different UUID was attached");
	}

	void olderBuildingWithoutReferenceStillLoads()
	{
		core::Building source("Legacy", 4, 2);
		source.pauseSimulation();
		auto yaml = serializeBuilding(source);
		auto const version = yaml.find("version: 14");
		require(version != std::string::npos, "The current Building schema was not version 14");
		yaml.replace(version, std::string("version: 14").size(), "version: 12");

		auto loaded = std::make_shared<core::Building>("Loading", 1, 1);
		loaded->pauseSimulation();
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData workData;
		require(loaded->deserialize(*reader, workData), "A version-12 Building did not load");
		require(!loaded->hasAgentBehaviourRegistryReference()
			&& !loaded->hasAttachedAgentBehaviourRegistry(),
			"An older Building invented an Agent behaviour registry");

		// Readers cap out at their own version, so a future document is refused
		// at the version boundary instead of dropping fields it does not know.
		auto future = serializeBuilding(source);
		auto const futureVersion = future.find("version: 14");
		future.replace(futureVersion, std::string("version: 14").size(), "version: 15");
		auto refused = std::make_shared<core::Building>("Loading", 1, 1);
		refused->pauseSimulation();
		auto futureReader = core::YamlSerializer::fromString(future);
		futureReader->deserialize();
		bool futureRefused{ false };
		try
		{
			(void)refused->deserialize(*futureReader, workData);
		}
		catch (std::exception const& error)
		{
			futureRefused = std::string(error.what()).find("Unsupported")
				!= std::string::npos;
		}
		require(futureRefused, "A future Building version was not refused at the boundary");
	}

	void selectionEnforcesPackageNamingAndDirectory()
	{
		TemporaryDirectory temporary;
		auto const project = temporary.path / "project";
		auto const otherProject = temporary.path / "other";
		std::filesystem::create_directories(project);
		std::filesystem::create_directories(otherProject);

		auto registry = core::AgentBehaviourRegistry::create();
		auto const packageDirectory = project / "shared.behaviours";
		writeText(manifestPath(packageDirectory), emptyManifestYaml(registry->getUuid()));
		registry->saveTo(manifestPath(packageDirectory).string());

		auto building = std::make_shared<core::Building>("Selection", 4, 2);
		building->pauseSimulation();
		auto const buildingPath = project / "selection.yaml";
		building->saveTo(buildingPath.string());
		auto selected = core::selectAndAttachAgentBehaviourRegistry(
			*building, buildingPath, packageDirectory);
		require(building->getAgentBehaviourRegistryPackageName() == "shared.behaviours",
			"Selection did not store only the package directory name");
		require(selected->getUuid() == registry->getUuid(),
			"Selection attached a registry with the wrong UUID");

		auto refused = std::make_shared<core::Building>("Refused selection", 4, 2);
		refused->pauseSimulation();
		auto const refusedPath = project / "refused.yaml";
		refused->saveTo(refusedPath.string());
		auto wrongNameDirectory = project / "registry";
		std::filesystem::create_directories(wrongNameDirectory);
		writeText(manifestPath(wrongNameDirectory), emptyManifestYaml(
			core::AgentBehaviourRegistry::create()->getUuid()));
		bool namingRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentBehaviourRegistry(
				*refused, refusedPath, wrongNameDirectory);
		}
		catch (std::exception const& error)
		{
			namingRefused = std::string(error.what()).find(".behaviours")
				!= std::string::npos;
		}
		require(namingRefused && !refused->hasAgentBehaviourRegistryReference()
			&& !refused->isModified(),
			"A package with the wrong directory name disturbed the Building");

		auto outsideDirectory = otherProject / "outside.behaviours";
		std::filesystem::create_directories(outsideDirectory);
		writeText(manifestPath(outsideDirectory), emptyManifestYaml(
			core::AgentBehaviourRegistry::create()->getUuid()));
		bool directoryRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentBehaviourRegistry(
				*refused, refusedPath, outsideDirectory);
		}
		catch (std::exception const& error)
		{
			directoryRefused = std::string(error.what()).find("same directory")
				!= std::string::npos;
		}
		require(directoryRefused && !refused->hasAgentBehaviourRegistryReference()
			&& !refused->isModified(),
			"A package outside the Building directory disturbed the Building");
	}

	void canonicalPackagesShareOneInstanceAndSameNamesNeverMerge()
	{
		TemporaryDirectory temporary;
		auto const firstDirectory = temporary.path / "first";
		auto const secondDirectory = temporary.path / "second";
		std::filesystem::create_directories(firstDirectory);
		std::filesystem::create_directories(secondDirectory);

		// Two independently authored packages declare a same-named behaviour.
		// Canonical package identity must keep them distinct: sharing merges
		// nothing, and each Building sees only its own package's definition.
		auto const firstYaml = ""
			"  version: 1\n"
			"  uuid: 123e4567-e89b-42d3-a456-426614174000\n"
			"  nextBehaviourId: 2\n"
			"  behaviours:\n"
			"    - id: 1\n"
			"      name: Schedule\n"
			"      revision: 7\n"
			"      source: first.lua\n";
		auto const secondYaml = ""
			"  version: 1\n"
			"  uuid: 123e4567-e89b-42d3-a456-426614174001\n"
			"  nextBehaviourId: 2\n"
			"  behaviours:\n"
			"    - id: 1\n"
			"      name: Schedule\n"
			"      revision: 3\n"
			"      source: second.lua\n";
		auto const firstPackage = firstDirectory / "shared.behaviours";
		auto const secondPackage = secondDirectory / "shared.behaviours";
		writeText(firstPackage / "first.lua", "-- first\n");
		writeText(firstPackage / "behaviours.yaml", firstYaml);
		writeText(secondPackage / "second.lua", "-- second\n");
		writeText(secondPackage / "behaviours.yaml", secondYaml);

		auto first = std::make_shared<core::Building>("First", 4, 2);
		first->pauseSimulation();
		auto second = std::make_shared<core::Building>("Second", 4, 2);
		second->pauseSimulation();
		auto const firstPath = firstDirectory / "first.yaml";
		auto const secondPath = secondDirectory / "second.yaml";
		first->saveTo(firstPath.string());
		second->saveTo(secondPath.string());

		auto sharedFirst = core::selectAndAttachAgentBehaviourRegistry(
			*first, firstPath, firstDirectory / "." / "shared.behaviours");
		auto distinct = core::selectAndAttachAgentBehaviourRegistry(
			*second, secondPath, secondPackage);
		require(sharedFirst != distinct,
			"Same-named packages in different directories shared an instance");

		auto const* firstBehaviour = sharedFirst->lookupAgentBehaviour(
			sharedFirst->getBehaviourIds().front());
		auto const* secondBehaviour = distinct->lookupAgentBehaviour(
			distinct->getBehaviourIds().front());
		require(firstBehaviour && secondBehaviour
			&& firstBehaviour->getName() == secondBehaviour->getName()
			&& firstBehaviour->getRevision() == 7
			&& secondBehaviour->getRevision() == 3
			&& firstBehaviour->getSourceModulePath() == "first.lua"
			&& secondBehaviour->getSourceModulePath() == "second.lua",
			"Same-named behaviour definitions were merged across packages");
		require(sharedFirst->hasLoadedBuilding(first.get())
			&& distinct->hasLoadedBuilding(second.get())
			&& !sharedFirst->hasLoadedBuilding(second.get()),
			"Loaded registries did not track exactly their dependent Buildings");

		first->saveTo(firstPath.string());
		second->saveTo(secondPath.string());
		first.reset();
		second.reset();
		sharedFirst.reset();
		distinct.reset();

		auto reopenedFirst = core::loadBuildingDocument(firstPath);
		auto reopenedSecond = core::loadBuildingDocument(secondPath);
		require(reopenedFirst->getAgentBehaviourRegistry()
			!= reopenedSecond->getAgentBehaviourRegistry(),
			"Two reopened Buildings sharing a behaviour name merged their registries");
	}

	void duplicateUuidAndInvalidPackagesAreTransactional()
	{
		TemporaryDirectory temporary;
		auto const firstDirectory = temporary.path / "first";
		auto const secondDirectory = temporary.path / "second";
		std::filesystem::create_directories(firstDirectory);
		std::filesystem::create_directories(secondDirectory);

		auto sourceRegistry = core::AgentBehaviourRegistry::create();
		auto const sourcePackage = firstDirectory / "registry.behaviours";
		auto const duplicatePackage = secondDirectory / "registry.behaviours";
		writeText(manifestPath(sourcePackage), emptyManifestYaml(sourceRegistry->getUuid()));
		sourceRegistry->saveTo(manifestPath(sourcePackage).string());
		std::filesystem::copy(sourcePackage, duplicatePackage,
			std::filesystem::copy_options::recursive);

		auto first = std::make_shared<core::Building>("First", 4, 2);
		first->pauseSimulation();
		auto second = std::make_shared<core::Building>("Second", 4, 2);
		second->pauseSimulation();
		auto const firstPath = firstDirectory / "first.yaml";
		auto const secondPath = secondDirectory / "second.yaml";
		first->saveTo(firstPath.string());
		second->saveTo(secondPath.string());
		auto loadedFirst = core::selectAndAttachAgentBehaviourRegistry(
			*first, firstPath, sourcePackage);

		bool duplicateRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentBehaviourRegistry(
				*second, secondPath, duplicatePackage);
		}
		catch (std::exception const& error)
		{
			duplicateRefused = std::string(error.what()).find("already loaded")
				!= std::string::npos;
		}
		require(duplicateRefused && !second->hasAgentBehaviourRegistryReference()
			&& !second->isModified()
			&& first->getAgentBehaviourRegistry() == loadedFirst,
			"A duplicate registry UUID disturbed loaded document state");

		// A refusal must not cache the duplicate path. Replacing that path with a
		// genuinely independent package makes it immediately selectable.
		auto independent = core::AgentBehaviourRegistry::create();
		std::filesystem::remove_all(duplicatePackage);
		writeText(manifestPath(duplicatePackage), emptyManifestYaml(independent->getUuid()));
		auto loadedSecond = core::selectAndAttachAgentBehaviourRegistry(
			*second, secondPath, duplicatePackage);
		require(loadedSecond != loadedFirst,
			"A duplicate-UUID refusal left a stale registry loaded at its path");

		auto refused = std::make_shared<core::Building>("Refused", 4, 2);
		refused->pauseSimulation();
		auto const refusedPath = firstDirectory / "refused.yaml";
		refused->saveTo(refusedPath.string());
		auto const brokenPackage = firstDirectory / "broken.behaviours";

		auto expectRefused = [&](std::string const& fixtureName,
			std::function<void()> prepare, char const* expected)
		{
			std::filesystem::remove_all(brokenPackage);
			prepare();
			bool wasRefused{ false };
			try
			{
				(void)core::selectAndAttachAgentBehaviourRegistry(
					*refused, refusedPath, brokenPackage);
			}
			catch (std::exception const& error)
			{
				wasRefused = std::string(error.what()).find(expected)
					!= std::string::npos;
			}
			require(wasRefused && !refused->hasAgentBehaviourRegistryReference()
				&& !refused->isModified(),
				fixtureName.c_str());
		};

		std::string const validUuid = core::AgentBehaviourRegistry::create()->getUuid();
		expectRefused("A malformed manifest disturbed the Building", [&]
		{
			std::filesystem::create_directories(brokenPackage);
			writeText(manifestPath(brokenPackage), "agentBehaviourRegistry: [not valid");
		}, "Could not load");
		expectRefused("An unsupported manifest schema disturbed the Building", [&]
		{
			writeText(manifestPath(brokenPackage), ""
				"  version: 2\n"
				"  uuid: " + validUuid + "\n"
				"  nextBehaviourId: 1\n"
				"  behaviours: []\n");
		}, "Unsupported");
		expectRefused("A missing manifest disturbed the Building", [&]
		{
			std::filesystem::create_directories(brokenPackage);
		}, "missing");
		expectRefused("A manifest naming a missing source module was accepted", [&]
		{
			writeText(manifestPath(brokenPackage), ""
				"  version: 1\n"
				"  uuid: " + validUuid + "\n"
				"  nextBehaviourId: 2\n"
				"  behaviours:\n"
				"    - id: 1\n"
				"      name: Ghost\n"
				"      revision: 1\n"
				"      source: ghost.lua\n");
		}, "missing");
		expectRefused("A manifest with a traversal source path was accepted", [&]
		{
			writeText(manifestPath(brokenPackage), ""
				"  version: 1\n"
				"  uuid: " + validUuid + "\n"
				"  nextBehaviourId: 2\n"
				"  behaviours:\n"
				"    - id: 1\n"
				"      name: Escape\n"
				"      revision: 1\n"
				"      source: ../outside.lua\n");
		}, "package");
		expectRefused("A manifest with an absolute source path was accepted", [&]
		{
			writeText(manifestPath(brokenPackage), ""
				"  version: 1\n"
				"  uuid: " + validUuid + "\n"
				"  nextBehaviourId: 2\n"
				"  behaviours:\n"
				"    - id: 1\n"
				"      name: Escape\n"
				"      revision: 1\n"
				"      source: /tmp/outside.lua\n");
		}, "absolute");
		expectRefused("A manifest with a duplicate behaviour name was accepted", [&]
		{
			writeText(manifestPath(brokenPackage), ""
				"  version: 1\n"
				"  uuid: " + validUuid + "\n"
				"  nextBehaviourId: 3\n"
				"  behaviours:\n"
				"    - id: 1\n"
				"      name: Schedule\n"
				"      revision: 1\n"
				"      source: one.lua\n"
				"    - id: 2\n"
				"      name: Schedule\n"
				"      revision: 1\n"
				"      source: two.lua\n");
			writeText(brokenPackage / "one.lua", "-- one\n");
			writeText(brokenPackage / "two.lua", "-- two\n");
		}, "unique");
	}

	void refusedBuildingLoadKeepsCurrentStateAndUnloadsCandidateRegistry()
	{
		TemporaryDirectory temporary;
		auto const sourceDirectory = temporary.path / "source";
		auto const destinationDirectory = temporary.path / "destination";
		std::filesystem::create_directories(sourceDirectory);
		std::filesystem::create_directories(destinationDirectory);
		auto const buildingPath = sourceDirectory / "referencing.yaml";
		auto const packageDirectory = sourceDirectory / "referencing.behaviours";

		{
			auto persisted = std::make_shared<core::Building>("Persisted", 4, 2);
		persisted->pauseSimulation();
			persisted->saveTo(buildingPath.string());
			(void)core::createAndAttachAgentBehaviourRegistry(*persisted, buildingPath);
			persisted->saveTo(buildingPath.string());
		}

		auto replacement = core::AgentBehaviourRegistry::create();
		writeText(manifestPath(packageDirectory), emptyManifestYaml(replacement->getUuid()));
		replacement->saveTo(manifestPath(packageDirectory).string());
		auto const replacementUuid = replacement->getUuid();
		auto const copiedPackage = destinationDirectory / "replacement.behaviours";
		std::filesystem::copy(packageDirectory, copiedPackage,
			std::filesystem::copy_options::recursive);
		replacement.reset();

		auto current = std::make_shared<core::Building>("Current", 7, 3);
		current->pauseSimulation();
		auto const* currentIdentity = current.get();
		bool mismatchRefused{ false };
		try
		{
			current = core::loadBuildingDocument(buildingPath);
		}
		catch (std::exception const& error)
		{
			mismatchRefused = std::string(error.what()).find("UUID mismatch")
				!= std::string::npos;
		}
		require(mismatchRefused && current.get() == currentIdentity
			&& current->getName() == "Current" && current->getCellsWide() == 7,
			"A refused Building load replaced or changed the current Building");

		// The replacement was parsed solely for the failed load above. It must no
		// longer count as loaded, so the same UUID at this independent path is valid.
		auto destination = std::make_shared<core::Building>("Destination", 4, 2);
		destination->pauseSimulation();
		auto const destinationPath = destinationDirectory / "destination.yaml";
		destination->saveTo(destinationPath.string());
		auto selected = core::selectAndAttachAgentBehaviourRegistry(
			*destination, destinationPath, copiedPackage);
		require(selected->getUuid() == replacementUuid,
			"A registry loaded only for a failed Building load remained referenced");
	}

	void failedAndOccupiedCreationLeavesNoReferenceOrDirectory()
	{
		TemporaryDirectory temporary;
		core::Building building("Atomic", 4, 2);
		building.pauseSimulation();
		auto const buildingPath = temporary.path / "atomic.yaml";
		building.saveTo(buildingPath.string());
		auto const packageDirectory
			= core::defaultAgentBehaviourRegistryPackagePath(buildingPath);

		// No-clobber: an occupied default directory refuses before any write.
		std::filesystem::create_directories(packageDirectory);
		bool occupiedRefused{ false };
		try
		{
			(void)core::createAndAttachAgentBehaviourRegistry(building, buildingPath);
		}
		catch (std::exception const& error)
		{
			occupiedRefused = std::string(error.what()).find("already exists")
				!= std::string::npos;
		}
		require(occupiedRefused && !building.hasAgentBehaviourRegistryReference(),
			"An occupied package directory did not refuse no-clobber creation");
		std::filesystem::remove_all(packageDirectory);

		core::YamlSerializer::setWriteFailureAfterBytesForTesting(1);
		bool refused{ false };
		try
		{
			(void)core::createAndAttachAgentBehaviourRegistry(building, buildingPath);
		}
		catch (std::exception const&)
		{
			refused = true;
		}
		core::YamlSerializer::setWriteFailureAfterBytesForTesting(0);
		require(refused, "The injected manifest write failure was not reported");
		require(!std::filesystem::exists(packageDirectory),
			"A failed registry creation left a partial package directory");
		require(!building.hasAgentBehaviourRegistryReference(),
			"A failed registry creation attached a nonexistent registry");
	}

	void definitionsPersistWithSchemasRevisionsAndModulePaths()
	{
		TemporaryDirectory temporary;
		auto building = std::make_shared<core::Building>("Definitions", 4, 2);
		building->pauseSimulation();
		auto const buildingPath = temporary.path / "definitions.yaml";
		building->saveTo(buildingPath.string());
		auto registry = core::createAndAttachAgentBehaviourRegistry(*building, buildingPath);
		auto const packageDirectory = temporary.path / "definitions.behaviours";
		writeText(packageDirectory / "schedule.lua", "-- schedule\n");
		writeText(packageDirectory / "wander.lua", "-- wander\n");

		std::string diagnostic;
		bool nameRulesOk{ true };
		for (auto const& invalid : std::vector<std::string>{ "", "  ", "\xc3\x28", std::string(64, 'a') })
		{
			if (core::AgentBehaviour::nameIsValid(invalid, &diagnostic)) nameRulesOk = false;
		}
		require(nameRulesOk && core::AgentBehaviour::nameIsValid("Schedule", &diagnostic)
			&& core::AgentBehaviour::nameIsValid("caf\xc3\xa9", &diagnostic),
			"Agent behaviour names did not enforce trim, UTF-8, and 63-byte rules");

		building->pauseSimulation();
		auto const schedule = registry->addAgentBehaviour(
			"Schedule", "schedule.lua", scheduleSchema());
		auto const wander = registry->addAgentBehaviour("Wander", "wander.lua", {});
		require(schedule.value == 1 && wander.value == 2
			&& registry->getNextBehaviourId() == 3,
			"Agent behaviours did not receive monotonic non-zero IDs");
		require(registry->lookupAgentBehaviour(schedule)->getRevision() == 1,
			"A new behaviour did not start at revision one");

		bool duplicateNameRefused{ false };
		try
		{
			(void)registry->addAgentBehaviour("Schedule", "wander.lua", {});
		}
		catch (std::exception const& error)
		{
			duplicateNameRefused = std::string(error.what()).find("already exists")
				!= std::string::npos;
		}
		require(duplicateNameRefused && registry->getBehaviourCount() == 2,
			"A duplicate case-sensitive behaviour name was accepted");

		bool missingModuleRefused{ false };
		try
		{
			(void)registry->addAgentBehaviour("Ghost", "ghost.lua", {});
		}
		catch (std::exception const& error)
		{
			missingModuleRefused = std::string(error.what()).find("missing")
				!= std::string::npos;
		}
		require(missingModuleRefused && registry->getBehaviourCount() == 2,
			"A behaviour naming a missing managed module was accepted");

		require(registry->renameAgentBehaviour(schedule, "Roam", &diagnostic),
			"A valid behaviour rename was refused");
		require(registry->lookupAgentBehaviour(schedule)->getName() == "Roam"
			&& registry->lookupAgentBehaviour(schedule)->getSourceModulePath() == "schedule.lua"
			&& registry->getNextBehaviourId() == 3,
			"Rename changed a behaviour's identity, module path, or allocator");

		registry->saveTo(manifestPath(packageDirectory).string());
		auto reopened = core::AgentBehaviourRegistry::loadFrom(
			manifestPath(packageDirectory).string());
		auto const* restored = reopened->lookupAgentBehaviour(schedule);
		require(restored && restored->getName() == "Roam"
			&& restored->getRevision() == 1
			&& restored->getSourceModulePath() == "schedule.lua"
			&& restored->getSchema().size() == 3
			&& restored->getSchema()[2].type == core::AgentBehaviourSchemaType::List
			&& restored->getSchema()[2].children.front().children.size() == 2
			&& restored->getSchema()[2].defaultValue
			&& core::agentBehaviourConfigurationGetIf<
				core::AgentBehaviourConfigurationList>(
					&*restored->getSchema()[2].defaultValue),
			"Behaviour identity, revision, module path, or schema did not survive save/load");
		require(reopened->getNextBehaviourId() == 3,
			"The behaviour allocator did not survive save/load");

		require(reopened->deleteAgentBehaviour(schedule, &diagnostic),
			"The deletion used to test ID non-reuse was refused");
		reopened->saveTo(manifestPath(packageDirectory).string());
		auto afterDelete = core::AgentBehaviourRegistry::loadFrom(
			manifestPath(packageDirectory).string());
		require(afterDelete->getNextBehaviourId() == 3,
			"Deleting and reopening moved the behaviour allocator backwards");
		auto const replacement = afterDelete->addAgentBehaviour(
			"Reserve", "schedule.lua", {});
		require(replacement.value == 3 && replacement != schedule,
			"A deleted AgentBehaviourId was reused");
	}

	void reloadValidatesAndSharesReplacementAcrossDependents()
	{
		TemporaryDirectory temporary;
		auto first = std::make_shared<core::Building>("First", 4, 2);
		first->pauseSimulation();
		auto second = std::make_shared<core::Building>("Second", 4, 2);
		second->pauseSimulation();
		auto const firstPath = temporary.path / "first.yaml";
		auto const secondPath = temporary.path / "second.yaml";
		first->saveTo(firstPath.string());
		second->saveTo(secondPath.string());
		auto registry = core::createAndAttachAgentBehaviourRegistry(*first, firstPath);
		auto const packageDirectory = temporary.path / "first.behaviours";
		writeText(packageDirectory / "schedule.lua",
			"return { api_version = 1, factory = function() return {} end }\n");

		first->pauseSimulation();
		(void)registry->addAgentBehaviour("Schedule", "schedule.lua", {});
		registry->saveTo(manifestPath(packageDirectory).string());

		// A second Building attaches the same canonical package and sees the
		// same shared instance.
		auto shared = core::selectAndAttachAgentBehaviourRegistry(
			*second, secondPath, packageDirectory);
		require(shared == registry,
			"A second Building did not share the canonical package instance");

		// External authoring adds a behaviour to the manifest; both dependents
		// observe it only after the explicit managed reload.
		auto const uuid = registry->getUuid();
		writeText(packageDirectory / "wander.lua",
			"return { api_version = 1, factory = function() return {} end }\n");
		writeText(manifestPath(packageDirectory), ""
			"  version: 1\n"
			"  uuid: " + uuid + "\n"
			"  nextBehaviourId: 3\n"
			"  behaviours:\n"
			"    - id: 1\n"
			"      name: Schedule\n"
			"      revision: 2\n"
			"      source: schedule.lua\n"
			"    - id: 2\n"
			"      name: Wander\n"
			"      revision: 1\n"
			"      source: wander.lua\n");
		require(registry->getBehaviourCount() == 1,
			"External manifest edits were visible before an explicit reload");

		second->pauseSimulation();
		std::string diagnostic;
		require(core::reloadAgentBehaviourRegistryDocument(
			registry, packageDirectory, &diagnostic),
			"The managed reload refused a valid externally edited package");
		require(registry->getBehaviourCount() == 2
			&& registry->lookupAgentBehaviour(core::AgentBehaviourId{ 1 })->getRevision() == 2,
			"The reload did not adopt the external definitions");
		require(first->getAgentBehaviourRegistry() == registry
			&& second->getAgentBehaviourRegistry() == registry,
			"The reload replaced a shared instance instead of updating it");

		// A dependent running simulation blocks the reload; the previous
		// definitions remain available.
		second->finishBuild();
		require(second->resumeSimulation(), "Could not resume dependent Building");
		writeText(manifestPath(packageDirectory), ""
			"  version: 1\n"
			"  uuid: " + uuid + "\n"
			"  nextBehaviourId: 3\n"
			"  behaviours:\n"
			"    - id: 1\n"
			"      name: Only\n"
			"      revision: 2\n"
			"      source: schedule.lua\n");
		require(!core::reloadAgentBehaviourRegistryDocument(
			registry, packageDirectory, &diagnostic)
			&& registry->getBehaviourCount() == 2,
			"A reload raced a running dependent simulation");
		second->pauseSimulation();
		require(core::reloadAgentBehaviourRegistryDocument(
			registry, packageDirectory, &diagnostic),
			"The reload did not proceed once every dependent was paused");
		require(registry->getBehaviourCount() == 1
			&& registry->getBehaviourName(core::AgentBehaviourId{ 1 }) == "Only",
			"The paused reload did not adopt the replacement definitions");

		// A dirty registry may not be reloaded over.
		first->pauseSimulation();
		(void)registry->renameAgentBehaviour(core::AgentBehaviourId{ 1 }, "Edited", &diagnostic);
		require(!core::reloadAgentBehaviourRegistryDocument(
			registry, packageDirectory, &diagnostic),
			"A dirty registry was reloaded over");
		require(registry->getBehaviourName(core::AgentBehaviourId{ 1 }) == "Edited",
			"A refused reload changed live definitions");
	}

	void hotReloadIsAtomicAcrossSourceHelpersAndDependentBuildings()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "atomic.behaviours";
		std::filesystem::create_directories(package);
		auto const uuid = std::string("123e4567-e89b-42d3-a456-426614174157");
		auto const manifest = ""
			"  version: 1\n"
			"  uuid: " + uuid + "\n"
			"  revision: 1\n"
			"  modules:\n"
			"    - name: helpers.reload\n"
			"      source: helper.lua\n"
			"  nextBehaviourId: 2\n"
			"  behaviours:\n"
			"    - id: 1\n"
			"      name: Atomic\n"
			"      revision: 1\n"
			"      source: atomic.lua\n"
			"      schema:\n"
			"        - name: expected\n"
			"          type: integer\n"
			"        - name: code\n"
			"          type: integer\n"
			"        - name: destination\n"
			"          type: marker\n";
		auto behaviourSource = [](std::string const& sourceRevision)
		{
			return "local helper = require('helpers.reload')\n"
				"local source_revision = '" + sourceRevision + "'\n"
				"return { api_version = 1, factory = function(configuration)\n"
				"  if configuration.expected ~= helper.expected then error('wrong expected value') end\n"
				"  return {\n"
				"    on_start = function(context)\n"
				"      context.log('start:' .. source_revision .. ':' .. helper.generation .. ':' .. context.random_integer(1, 1000000))\n"
				"      context.set_timer('preserved', 3)\n"
				"      local moved = context.move_to(configuration.destination)\n"
				"      if not moved.accepted then error(moved.status) end\n"
				"    end,\n"
				"    on_timer = function(name, context) context.log('timer:' .. source_revision .. ':' .. helper.generation .. ':' .. name) end,\n"
				"    on_stop = function(reason, context) context.log('stop:' .. source_revision .. ':' .. reason) end\n"
				"  }\n"
				"end }\n";
		};
		writeText(manifestPath(package), manifest);
		writeText(package / "helper.lua",
			"return { expected = 7, generation = 'v1' }\n");
		writeText(package / "atomic.lua", behaviourSource("v1"));

		struct Fixture
		{
			std::shared_ptr<core::Building> building;
			std::filesystem::path path;
			std::vector<core::AgentId> agents;
		};
		auto makeBuilding = [&](std::string name, std::string filename,
			unsigned agentCount)
		{
			Fixture fixture;
			fixture.building = std::make_shared<core::Building>(std::move(name), 10, 2);
			auto const room = fixture.building->addRoom("Room", 0, 0, 0, 10, 1);
			fixture.building->addSectorMarker(room, 0, 8.5f, "Destination");
			fixture.building->finishBuild();
			for (unsigned index = 0; index < agentCount; ++index)
				fixture.agents.push_back(fixture.building->createAgent(
					"Agent " + std::to_string(index + 1), room, 0,
					0.5f + static_cast<float>(index)));
			fixture.building->pauseSimulation();
			fixture.path = temporary.path / filename;
			fixture.building->saveTo(fixture.path.string());
			return fixture;
		};
		auto alpha = makeBuilding("Alpha", "alpha.yaml", 2);
		auto zulu = makeBuilding("Zulu", "zulu.yaml", 1);

		// Attach in reverse display order. Reload diagnostics must still use stable
		// Building-name order and stable Agent-ID order.
		auto registry = core::selectAndAttachAgentBehaviourRegistry(
			*zulu.building, zulu.path, package);
		require(core::selectAndAttachAgentBehaviourRegistry(
				*alpha.building, alpha.path, package) == registry,
			"Atomic reload dependents did not share one registry");
		auto const behaviour = core::AgentBehaviourId{ 1 };
		auto assign = [&](Fixture& fixture, uint64_t& code)
		{
			for (auto agent : fixture.agents)
			{
				require(fixture.building->setAgentBehaviourAssignment(agent, behaviour, 1, {
					{ "expected", int64_t{ 7 } },
					{ "code", static_cast<int64_t>(code++) },
					{ "destination", fixture.building->getMarkerIds().front() }
				}), "Could not assign an atomic reload fixture");
			}
			fixture.building->saveTo(fixture.path.string());
		};
		uint64_t code = 1;
		assign(alpha, code);
		assign(zulu, code);
		require(!registry->isModified() && !alpha.building->isModified()
			&& !zulu.building->isModified(),
			"Atomic reload fixtures did not start with clean documents");

		auto runStartBoundary = [&](Fixture& fixture)
		{
			require(fixture.building->resumeSimulation(),
				"Could not resume an atomic reload fixture");
			require(fixture.building->advanceTick(),
				"A restarted atomic reload instance failed");
			fixture.building->pauseSimulation();
		};
		runStartBoundary(alpha);
		runStartBoundary(zulu);
		(void)core::consumeLogMessages();

		writeText(package / "helper.lua",
			"return { expected = 7, generation = 'v2' }\n");
		writeText(package / "atomic.lua", behaviourSource("v2"));
		std::string diagnostic;
		std::vector<core::AgentBehaviourReloadDiagnostic> reloadDiagnostics;

		// One running dependent refuses the transaction before any scratch module
		// executes or any live state changes.
		require(zulu.building->resumeSimulation(),
			"Could not run the dependent used by the reload refusal check");
		require(!core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics)
			&& diagnostic.find("Zulu") != std::string::npos,
			"A reload was not refused while a dependent Building was running");
		zulu.building->pauseSimulation();

		require(core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics)
			&& reloadDiagnostics.empty(),
			"Valid source/helper reload did not commit atomically");
		auto stopMessages = core::consumeLogMessages();
		auto const stopCount = std::count_if(stopMessages.begin(), stopMessages.end(),
			[](core::LogMessage const& message)
			{
				return message.msg == "stop:v1:reload";
			});
		require(stopCount == 3,
			"Successful reload did not call read-only teardown on every old instance");
		for (auto agent : alpha.agents)
			require(!alpha.building->lookupAgent(agent).entity->getPath(),
				"Successful reload retained old behaviour movement");

		auto collectStarts = []
		{
			auto messages = core::consumeLogMessages();
			std::vector<std::string> starts;
			for (auto const& message : messages)
				if (message.msg.starts_with("start:v2:v2:")) starts.push_back(message.msg);
			return starts;
		};
		runStartBoundary(alpha);
		runStartBoundary(zulu);
		auto const firstRestart = collectStarts();
		require(firstRestart.size() == 3,
			"Reloaded instances were not recreated from all authored configurations");

		auto const historyState = gBuildingDocumentHistory.currentStateId();
		auto const undoCount = gBuildingDocumentHistory.undoCount();
		auto const redoCount = gBuildingDocumentHistory.redoCount();
		auto assertRollback = [&](std::string const& candidate,
			core::AgentBehaviourReloadDiagnosticScope expectedScope)
		{
			writeText(package / "atomic.lua", candidate);
			reloadDiagnostics.clear();
			require(!core::reloadAgentBehaviourRegistryDocument(registry, package,
					&diagnostic, &reloadDiagnostics)
				&& !reloadDiagnostics.empty()
				&& reloadDiagnostics.front().scope == expectedScope
				&& registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
					== core::AgentBehaviourModuleStatus::Loaded
				&& !registry->isModified() && !alpha.building->isModified()
				&& !zulu.building->isModified()
				&& gBuildingDocumentHistory.currentStateId() == historyState
				&& gBuildingDocumentHistory.undoCount() == undoCount
				&& gBuildingDocumentHistory.redoCount() == redoCount,
				"A failed reload changed live registry/runtime document state or history");
		};
		assertRollback("return { api_version = 1, factory = function( }\n",
			core::AgentBehaviourReloadDiagnosticScope::Module);
		assertRollback("require('helpers.missing')\nreturn { api_version = 1, factory = function() return {} end }\n",
			core::AgentBehaviourReloadDiagnosticScope::Module);
		assertRollback("return { api_version = 2, factory = function() return {} end }\n",
			core::AgentBehaviourReloadDiagnosticScope::Module);
		assertRollback("while true do end\n",
			core::AgentBehaviourReloadDiagnosticScope::Module);

		writeText(package / "atomic.lua", behaviourSource("v2"));
		writeText(package / "helper.lua", "return { broken = function( }\n");
		reloadDiagnostics.clear();
		require(!core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics)
			&& reloadDiagnostics.size() >= 2
			&& std::all_of(reloadDiagnostics.begin(), reloadDiagnostics.end(),
				[](core::AgentBehaviourReloadDiagnostic const& item)
				{
					return item.scope
						== core::AgentBehaviourReloadDiagnosticScope::Module;
				})
			&& !registry->isModified() && !alpha.building->isModified()
			&& !zulu.building->isModified(),
			"Helper-graph failures were not aggregated without mutation");
		writeText(package / "helper.lua",
			"return { expected = 7, generation = 'v2' }\n");

		writeText(package / "atomic.lua", ""
			"return { api_version = 1, factory = function(configuration)\n"
			"  error('factory rejected code ' .. configuration.code)\n"
			"end }\n");
		reloadDiagnostics.clear();
		require(!core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics)
			&& reloadDiagnostics.size() == 3
			&& reloadDiagnostics[0].scope
				== core::AgentBehaviourReloadDiagnosticScope::Agent
			&& reloadDiagnostics[0].buildingName == "Alpha"
			&& reloadDiagnostics[0].agent == alpha.agents[0]
			&& reloadDiagnostics[1].buildingName == "Alpha"
			&& reloadDiagnostics[1].agent == alpha.agents[1]
			&& reloadDiagnostics[2].buildingName == "Zulu"
			&& reloadDiagnostics[2].agent == zulu.agents[0],
			"Per-configuration factory failures were not aggregated in stable Building/Agent order");

		// All failed attempts leave the v2 instances and their timers intact. A
		// restart would emit start again and postpone these timers.
		for (auto* fixture : { &alpha, &zulu })
		{
			require(fixture->building->resumeSimulation(),
				"Could not resume after a refused reload");
			require(fixture->building->advanceTicks(3),
				"The previous runtime failed after a refused reload");
			fixture->building->pauseSimulation();
		}
		auto retainedMessages = core::consumeLogMessages();
		auto retainedTimers = std::count_if(retainedMessages.begin(), retainedMessages.end(),
			[](core::LogMessage const& message)
			{
				return message.msg == "timer:v2:v2:preserved";
			});
		auto repeatedStarts = std::count_if(retainedMessages.begin(), retainedMessages.end(),
			[](core::LogMessage const& message)
			{
				return message.msg.starts_with("start:");
			});
		require(retainedTimers == 3 && repeatedStarts == 0,
			"Failed reload did not preserve the previous live instance state");

		// Re-adopting the same valid revision restarts deterministic random streams
		// and produces the same per-Agent startup outcomes.
		writeText(package / "atomic.lua", behaviourSource("v2"));
		require(core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics),
			"Could not recover from refused reload candidates");
		(void)core::consumeLogMessages(); // successful-reload teardown
		runStartBoundary(alpha);
		runStartBoundary(zulu);
		auto const secondRestart = collectStarts();
		require(secondRestart == firstRestart,
			"Repeated successful reloads produced nondeterministic restarted outcomes");
	}

	void packageContainmentAndLifecycle()
	{
		TemporaryDirectory temporary;
		auto building = std::make_shared<core::Building>("Containment", 4, 2);
		building->pauseSimulation();
		auto const path = temporary.path / "containment.yaml";
		building->saveTo(path.string());
		auto registry = core::createAndAttachAgentBehaviourRegistry(*building, path);
		auto const package = core::defaultAgentBehaviourRegistryPackagePath(path);
		writeText(package / "nested" / "source.lua",
			"return {api_version=1,factory=function() return {on_start=function() error('must never execute') end} end}\n");
		auto id = registry->addAgentBehaviour("  Schedule  ", "nested/source.lua", {});
		require(registry->getBehaviourName(id) == "Schedule", "Authored names were not trimmed");
		registry->saveTo(manifestPath(package).string());
		auto const valid = readText(manifestPath(package));
		std::string diagnostic;
		require(core::reloadAgentBehaviourRegistryDocument(registry, package, &diagnostic)
			&& registry->lookupAgentBehaviour(id)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"Nested managed source was refused or an Agent callback executed");
		building->saveTo(path.string());
		building->resetSimulation();
		require(building->getAgentBehaviourRegistry() == registry
			&& registry->hasLoadedBuilding(building.get()), "Reset lost the registry attachment");

		// Escaping symlinks are rejected after canonicalization, including the
		// manifest itself. Windows may not grant symlink creation privileges.
		writeText(temporary.path / "outside.lua", "error('outside')\n");
		std::error_code error;
		std::filesystem::remove(package / "nested" / "source.lua");
		std::filesystem::create_symlink(temporary.path / "outside.lua",
			package / "nested" / "source.lua", error);
		if (!error)
		{
			require(!core::reloadAgentBehaviourRegistryDocument(registry, package, &diagnostic)
				&& registry->getBehaviourCount() == 1, "A symlink escaped the package");
			std::filesystem::remove(package / "nested" / "source.lua");
		}
		writeText(package / "nested" / "source.lua",
			"return {api_version=1,factory=function() return {} end}\n");
		writeText(temporary.path / "outside.yaml", valid);
		std::filesystem::remove(manifestPath(package));
		error.clear();
		std::filesystem::create_symlink(temporary.path / "outside.yaml", manifestPath(package), error);
		if (!error)
		{
			require(!core::reloadAgentBehaviourRegistryDocument(registry, package, &diagnostic),
				"A manifest symlink escaped the package");
			std::filesystem::remove(manifestPath(package));
		}
		writeText(manifestPath(package), valid);

		auto regressed = valid;
		auto const allocator = regressed.find("nextBehaviourId: 2");
		require(allocator != std::string::npos, "Missing fixture allocator");
		regressed.replace(allocator, std::string("nextBehaviourId: 2").size(), "nextBehaviourId: 1");
		writeText(manifestPath(package), regressed);
		require(!core::reloadAgentBehaviourRegistryDocument(registry, package, &diagnostic)
			&& registry->getNextBehaviourId() == 2, "Malformed allocator changed live state");
		writeText(manifestPath(package), valid);

		building->finishBuild();
		require(building->resumeSimulation(), "Could not resume lifecycle fixture");
		bool refused = false;
		try { building->detachAgentBehaviourRegistry(); }
		catch (std::exception const&) { refused = true; }
		require(refused && building->hasAttachedAgentBehaviourRegistry(), "Running detach succeeded");
		building->pauseSimulation();

		// Exercise the actual extracted panel in a CPU-side ImGui context.
		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800, 600);
		unsigned char* pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		ImGui::NewFrame();
		ImGui::Begin("Behaviour registry smoke");
		require(!renderBehavioursPanel(building, path.string()), "Inspection edited the Building");
		ImGui::End();
		ImGui::Render();
		ImGui::DestroyContext();

		building->detachAgentBehaviourRegistry();
		require(!registry->hasLoadedBuildings()
			&& readText(manifestPath(package)) == valid, "Detach changed the package or kept its dependent");
		require(core::unloadAgentBehaviourRegistryDocumentIfUnused(registry), "Unused package did not unload");
	}

	void unsavedBuildingDocumentRefusesManagedOperations()
	{
		TemporaryDirectory temporary;
		core::Building building("Unsaved", 4, 2);
		building.pauseSimulation();
		auto const buildingPath = temporary.path / "unsaved.yaml";
		bool selectRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentBehaviourRegistry(
				building, buildingPath, temporary.path / "any.behaviours");
		}
		catch (std::exception const& error)
		{
			selectRefused = std::string(error.what()).find("Save")
				!= std::string::npos;
		}
		require(selectRefused && !building.hasAgentBehaviourRegistryReference(),
			"Selecting a package for an unsaved Building was enabled");
	}
}

void runAgentBehaviourRegistrySmokeChecks()
{
	savedBuildingCreatesAndReopensAdjacentPackage();
	olderBuildingWithoutReferenceStillLoads();
	selectionEnforcesPackageNamingAndDirectory();
	canonicalPackagesShareOneInstanceAndSameNamesNeverMerge();
	duplicateUuidAndInvalidPackagesAreTransactional();
	refusedBuildingLoadKeepsCurrentStateAndUnloadsCandidateRegistry();
	failedAndOccupiedCreationLeavesNoReferenceOrDirectory();
	definitionsPersistWithSchemasRevisionsAndModulePaths();
	reloadValidatesAndSharesReplacementAcrossDependents();
	hotReloadIsAtomicAcrossSourceHelpersAndDependentBuildings();
	unsavedBuildingDocumentRefusesManagedOperations();
	packageContainmentAndLifecycle();
}
