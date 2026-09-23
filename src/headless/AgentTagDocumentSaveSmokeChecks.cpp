// Dependency-ordered saves and independent cross-directory Save As copies,
// tickets #142 and #144.

#include "TagsPanel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

void runAgentTagDocumentSaveSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path;

		TemporaryDirectory()
		{
			path = std::filesystem::temp_directory_path()
				/ ("promethium-fermide-document-save-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}

		~TemporaryDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	};

	std::string readFile(std::filesystem::path const& path)
	{
		std::ifstream input(path, std::ios::binary);
		return { std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>() };
	}

	std::string serializeRegistry(core::AgentTagRegistry const& registry)
	{
		auto serializer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		registry.serialize(*serializer, work);
		serializer->serialize();
		return serializer->getSerializedString();
	}

	std::string serializeWorld(core::World const& world)
	{
		auto serializer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*serializer, work);
		serializer->serialize();
		return serializer->getSerializedString();
	}

	struct SavedFixture
	{
		TemporaryDirectory temporary;
		std::filesystem::path worldPath;
		std::filesystem::path registryPath;
		std::shared_ptr<core::World> world;
		std::shared_ptr<core::AgentTagRegistry> registry;
		DocumentHistory worldHistory;
		core::AgentTagId tag{};

		explicit SavedFixture(std::string const& stem)
			: worldPath(temporary.path / (stem + ".world.yaml")),
			registryPath(temporary.path / (stem + ".tags.yaml")),
			world(std::make_shared<core::World>(stem, 8, 2))
		{
			auto const corridor = world->addCorridor(0, 0, 7);
			world->finishBuild();
			world->saveTo(worldPath.string());
			registry = core::createAndAttachAgentTagRegistry(*world, worldPath);
			world->pauseSimulation();

			std::string diagnostic;
			tag = commitAgentTagAdd(registry, "walkers", diagnostic);
			require(tag && commitAgentTagWalkSpeedModifierAdd(
				registry, tag, diagnostic), diagnostic);
			auto const agent = world->createAgent("Walker", corridor, 0, 2.0f);
			require(world->assignAgentTag(agent, tag, &diagnostic), diagnostic);
			require(saveWorldDocument(target(), &diagnostic),
				"Could not establish a clean save fixture: " + diagnostic);
			require(!agentTagRegistryIsModified(registry)
				&& !world->isModified() && !worldHistory.isModified(),
				"The initial registry and World save did not clean independent markers");
		}

		~SavedFixture()
		{
			forgetAgentTagRegistryDocument(registry);
		}

		WorldDocumentSaveTarget target()
		{
			return { world, worldPath.string(), registryPath.string(),
				&worldHistory };
		}

		void editModifier(float value)
		{
			std::string diagnostic;
			require(commitAgentTagWalkSpeedModifierEdit(
				registry, tag, { value, value }, diagnostic), diagnostic);
			worldHistory.commit(worldHistory.capture("World before edit"));
			require(agentTagRegistryIsModified(registry)
				&& world->isModified() && worldHistory.isModified(),
				"A modifier edit did not dirty registry and World markers");
		}
	};

	void worldSaveWritesRegistryFirstAndCleansIndependently()
	{
		SavedFixture fixture("single");
		fixture.editModifier(1.1f);
		std::string diagnostic;
		require(saveWorldDocument(fixture.target(), &diagnostic), diagnostic);
		require(!agentTagRegistryIsModified(fixture.registry)
			&& !fixture.world->isModified()
			&& !fixture.worldHistory.isModified(),
			"A successful dependency-ordered save did not clean each document marker");

		auto diskRegistry = core::AgentTagRegistry::loadFrom(
			fixture.registryPath.string());
		auto reopened = core::loadWorldDocument(fixture.worldPath);
		require(reopened->hasAttachedAgentTagRegistry()
			&& diskRegistry->getAgentTagWalkSpeedModifier(fixture.tag)->range
				== core::AgentModifierRange{ 1.1f, 1.1f }
			&& !reopened->isModified(),
			"The saved modifier definition and dependent samples did not round-trip cleanly");
	}

	void registryFailureBlocksWorldAndPreservesDirtyState()
	{
		SavedFixture fixture("failure");
		auto const worldOnDisk = readFile(fixture.worldPath);
		auto const registryOnDisk = readFile(fixture.registryPath);
		fixture.editModifier(1.1f);

		core::YamlSerializer::setWriteFailureAfterBytesForTesting(1);
		std::string diagnostic;
		auto const saved = saveWorldDocument(fixture.target(), &diagnostic);
		core::YamlSerializer::setWriteFailureAfterBytesForTesting(0);
		require(!saved && diagnostic.find("Agent tag registry") != std::string::npos,
			"An injected registry failure was not reported as a registry save failure");
		require(readFile(fixture.registryPath) == registryOnDisk
			&& readFile(fixture.worldPath) == worldOnDisk,
			"A registry failure changed the registry or allowed its World onto disk");
		require(agentTagRegistryIsModified(fixture.registry)
			&& fixture.world->isModified()
			&& fixture.worldHistory.isModified(),
			"A registry failure cleared a registry or World dirty marker");
	}

	void saveAllCompletesRegistryPhaseBeforeAnyWorld()
	{
		SavedFixture first("first");
		SavedFixture second("second");
		first.editModifier(1.1f);
		second.editModifier(1.2f);
		auto const firstWorldOnDisk = readFile(first.worldPath);
		auto const secondWorldOnDisk = readFile(second.worldPath);

		auto firstTarget = first.target();
		auto secondTarget = second.target();
		// A directory cannot be atomically replaced by the registry YAML file. It
		// gives the second registry a deterministic failure after the first one has
		// succeeded, proving that neither World is written between those saves.
		auto const refusedDestination = second.temporary.path / "unwritable-registry";
		std::filesystem::create_directory(refusedDestination);
		secondTarget.registryFilepath = refusedDestination.string();
		std::string diagnostic;
		require(!saveAllDocuments({ firstTarget, secondTarget }, &diagnostic),
			"Save All unexpectedly accepted an unwritable registry destination");
		require(!agentTagRegistryIsModified(first.registry)
			&& agentTagRegistryIsModified(second.registry),
			"Successful and failed registry saves did not update their markers independently");
		require(first.world->isModified() && second.world->isModified()
			&& first.worldHistory.isModified()
			&& second.worldHistory.isModified(),
			"A failed registry phase cleared a dependent World marker");
		require(readFile(first.worldPath) == firstWorldOnDisk
			&& readFile(second.worldPath) == secondWorldOnDisk,
			"Save All wrote a World before every dirty registry had succeeded");
	}

	void sameDirectorySaveAsRetainsRegistryReference()
	{
		SavedFixture fixture("same-directory");
		auto const sourceRegistry = fixture.registry;
		auto const sourceUuid = sourceRegistry->getUuid();
		auto const sourceFilename = fixture.world->getAgentTagRegistryFilename();
		auto const destination = fixture.temporary.path / "same-directory-copy.world.yaml";

		std::string diagnostic;
		require(saveWorldDocument({ fixture.world, destination.string(),
			fixture.registryPath.string(), &fixture.worldHistory }, &diagnostic),
			"Same-directory Save As failed: " + diagnostic);
		require(fixture.world->getAgentTagRegistry() == sourceRegistry
			&& fixture.world->getExpectedAgentTagRegistryUuid() == sourceUuid
			&& fixture.world->getAgentTagRegistryFilename() == sourceFilename,
			"Same-directory Save As changed the existing registry reference");

		auto reopened = core::loadWorldDocument(destination);
		require(reopened->getAgentTagRegistry() == sourceRegistry
			&& reopened->getExpectedAgentTagRegistryUuid() == sourceUuid,
			"A same-directory Save As did not reopen against the shared source registry");
	}

	void crossDirectorySaveAsCopiesEquivalentIndependentRegistry()
	{
		SavedFixture fixture("cross-directory");
		std::string diagnostic;
		auto const retired = commitAgentTagAdd(fixture.registry, "retired", diagnostic);
		require(static_cast<bool>(retired), diagnostic);
		require(commitAgentTagColourAdd(fixture.registry, retired, diagnostic), diagnostic);
		require(commitAgentTagDelete(fixture.registry, retired, diagnostic), diagnostic);
		require(commitAgentTagColourAdd(fixture.registry, fixture.tag, diagnostic), diagnostic);
		require(commitAgentTagColourEdit(fixture.registry, fixture.tag,
			{ 12, 34, 56 }, diagnostic), diagnostic);
		require(commitAgentTagWalkSpeedModifierEdit(fixture.registry, fixture.tag,
			{ 1.1f, 1.1f }, diagnostic), diagnostic);
		require(saveWorldDocument(fixture.target(), &diagnostic), diagnostic);

		auto const sourceRegistry = fixture.registry;
		auto const sourceUuid = sourceRegistry->getUuid();
		auto const sourceRegistryYaml = serializeRegistry(*sourceRegistry);
		auto const sourceWorldYaml = serializeWorld(*fixture.world);
		auto copiedWorld = core::loadWorldDocument(fixture.worldPath);
		copiedWorld->pauseSimulation();
		DocumentHistory copiedHistory;
		copiedHistory.markSaved();

		auto const destinationDirectory = fixture.temporary.path / "copy";
		std::filesystem::create_directory(destinationDirectory);
		auto const destinationWorld = destinationDirectory / "renamed.world.yaml";
		auto const destinationRegistry = destinationDirectory
			/ fixture.world->getAgentTagRegistryFilename();
		require(saveWorldDocument({ copiedWorld, destinationWorld.string(),
			fixture.registryPath.string(), &copiedHistory }, &diagnostic),
			"Cross-directory Save As failed: " + diagnostic);

		auto const copiedRegistry = copiedWorld->getAgentTagRegistry();
		require(copiedRegistry && copiedRegistry != sourceRegistry
			&& copiedRegistry->getUuid() != sourceUuid,
			"Cross-directory Save As did not attach an independent registry UUID");
		require(std::filesystem::is_regular_file(destinationRegistry)
			&& copiedWorld->getAgentTagRegistryFilename()
				== fixture.world->getAgentTagRegistryFilename()
			&& copiedWorld->getExpectedAgentTagRegistryUuid()
				== copiedRegistry->getUuid(),
			"The copied World does not reference its adjacent registry copy");
		require(copiedRegistry->hasEquivalentDefinitions(*sourceRegistry)
			&& copiedRegistry->getAgentTagIds() == sourceRegistry->getAgentTagIds()
			&& copiedRegistry->getNextAgentTagId()
				== sourceRegistry->getNextAgentTagId()
			&& copiedRegistry->getNextPropertyRevision()
				== sourceRegistry->getNextPropertyRevision(),
			"The registry copy lost tag identities, definitions, revisions, or allocator state");
		require(copiedWorld->getAgentTagAssignmentCount()
			== fixture.world->getAgentTagAssignmentCount(),
			"The copied World lost Agent tag assignments");

		auto reopenedCopy = core::loadWorldDocument(destinationWorld);
		reopenedCopy->pauseSimulation();
		require(reopenedCopy->getAgentTagRegistry() == copiedRegistry
			&& reopenedCopy->getAgentTagAssignmentCount()
				== copiedWorld->getAgentTagAssignmentCount(),
			"The copied World and registry did not round-trip together");

		require(commitAgentTagRename(copiedRegistry, fixture.tag,
			"commuters", diagnostic), diagnostic);
		require(commitAgentTagWalkSpeedModifierEdit(copiedRegistry, fixture.tag,
			{ 1.2f, 1.2f }, diagnostic), diagnostic);
		require(sourceRegistry->getAgentTagName(fixture.tag) == "walkers"
			&& sourceRegistry->getAgentTagWalkSpeedModifier(fixture.tag)->range
				== core::AgentModifierRange{ 1.1f, 1.1f }
			&& serializeRegistry(*sourceRegistry) == sourceRegistryYaml
			&& serializeWorld(*fixture.world) == sourceWorldYaml,
			"Editing the copied registry affected the original registry or World");
	}

	void registryCollisionLeavesSourceAndDestinationUnchanged()
	{
		SavedFixture fixture("collision");
		std::string diagnostic;
		require(commitAgentTagRename(fixture.registry, fixture.tag,
			"commuters", diagnostic), diagnostic);
		fixture.world->markModified();
		fixture.worldHistory.commit(
			fixture.worldHistory.capture("World before collision Save As"));

		auto const sourceRegistryOnDisk = readFile(fixture.registryPath);
		auto const sourceWorldOnDisk = readFile(fixture.worldPath);
		auto const sourceRegistryInMemory = serializeRegistry(*fixture.registry);
		auto const sourceWorldInMemory = serializeWorld(*fixture.world);
		auto const sourceRegistry = fixture.registry;
		auto const sourceHistoryState = fixture.worldHistory.currentStateId();

		auto const destinationDirectory = fixture.temporary.path / "occupied";
		std::filesystem::create_directory(destinationDirectory);
		auto const destinationWorld = destinationDirectory / "copy.world.yaml";
		auto const destinationRegistry = destinationDirectory
			/ fixture.world->getAgentTagRegistryFilename();
		{
			std::ofstream registryOutput(destinationRegistry, std::ios::binary);
			registryOutput << "occupied registry";
			std::ofstream worldOutput(destinationWorld, std::ios::binary);
			worldOutput << "occupied world";
		}
		auto const destinationRegistryBefore = readFile(destinationRegistry);
		auto const destinationWorldBefore = readFile(destinationWorld);

		require(!saveWorldDocument({ fixture.world,
			destinationWorld.string(), fixture.registryPath.string(),
			&fixture.worldHistory }, &diagnostic),
			"Save As unexpectedly overwrote an existing destination registry");
		require(diagnostic.find("already exists") != std::string::npos,
			"A destination registry collision did not produce a useful diagnostic");
		require(readFile(destinationRegistry) == destinationRegistryBefore
			&& readFile(destinationWorld) == destinationWorldBefore,
			"A registry collision changed destination state");
		require(readFile(fixture.registryPath) == sourceRegistryOnDisk
			&& readFile(fixture.worldPath) == sourceWorldOnDisk
			&& serializeRegistry(*fixture.registry) == sourceRegistryInMemory
			&& serializeWorld(*fixture.world) == sourceWorldInMemory
			&& fixture.world->getAgentTagRegistry() == sourceRegistry
			&& fixture.worldHistory.currentStateId() == sourceHistoryState
			&& agentTagRegistryIsModified(fixture.registry)
			&& fixture.worldHistory.isModified(),
			"A registry collision changed source disk, document, reference, or dirty state");
	}

	void closePromptNamesOnlyTheDirtyDocumentKinds()
	{
		SavedFixture fixture("prompt");
		std::string diagnostic;
		require(commitAgentTagRename(fixture.registry, fixture.tag,
			"commuters", diagnostic), diagnostic);
		auto prompt = unsavedDocumentPromptText(fixture.target());
		require(prompt.find("Agent tag registry: prompt.tags.yaml")
				!= std::string::npos
			&& prompt.find("\n- World:") == std::string::npos,
			"The close prompt did not identify a registry-only dirty state separately");

		fixture.world->markModified();
		fixture.worldHistory.commit(
			fixture.worldHistory.capture("World before prompt edit"));
		prompt = unsavedDocumentPromptText(fixture.target());
		require(prompt.find("World: prompt.world.yaml") != std::string::npos
			&& prompt.find("Agent tag registry: prompt.tags.yaml")
				!= std::string::npos,
			"The close/exit prompt did not name both independently dirty documents");
	}
}

void runAgentTagDocumentSaveSmokeChecks()
{
	worldSaveWritesRegistryFirstAndCleansIndependently();
	registryFailureBlocksWorldAndPreservesDirtyState();
	saveAllCompletesRegistryPhaseBeforeAnyWorld();
	sameDirectorySaveAsRetainsRegistryReference();
	crossDirectorySaveAsCopiesEquivalentIndependentRegistry();
	registryCollisionLeavesSourceAndDestinationUnchanged();
	closePromptNamesOnlyTheDirtyDocumentKinds();
}
