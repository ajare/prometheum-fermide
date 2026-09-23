// Agent behaviour clipboard and Save As portability checks for #163.

#include "AgentClipboard.h"
#include "DocumentEdit.h"
#include "TagsPanel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

void runAgentBehaviourPortabilitySmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path = std::filesystem::temp_directory_path()
			/ ("promethium-behaviour-portability-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()));
		TemporaryDirectory() { std::filesystem::create_directories(path); }
		~TemporaryDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	};

	void writeFile(std::filesystem::path const& path, std::string const& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary);
		output << text;
	}

	struct World
	{
		std::shared_ptr<core::World> world;
		uint32_t room{};
		core::MarkerId alpha{};
		core::MarkerId beta{};
	};

	World makeWorld(std::string const& name,
		std::shared_ptr<core::AgentBehaviourRegistry> const& registry,
		bool shiftMarkerIds)
	{
		World result;
		result.world = std::make_shared<core::World>(name, 12, 2);
		result.room = result.world->addRoom("Room", 0, 0, 0, 12, 1);
		if (shiftMarkerIds)
		{
			result.world->addSectorMarker(result.room, 0, 1.5f, "Other");
			result.world->removeSectorMarker(result.room, 0);
		}
		result.world->addSectorMarker(result.room, 0, 8.5f, "Alpha");
		result.alpha = result.world->getMarkerIds().back();
		result.world->addSectorMarker(result.room, 0, 9.5f, "Beta");
		result.beta = result.world->getMarkerIds().back();
		result.world->finishBuild();
		result.world->pauseSimulation();
		result.world->attachAgentBehaviourRegistry(name + ".behaviours", registry);
		return result;
	}

	std::vector<core::AgentBehaviourSchemaField> schema()
	{
		return {
			{ "enabled", core::AgentBehaviourSchemaType::Boolean },
			{ "count", core::AgentBehaviourSchemaType::Integer },
			{ "weight", core::AgentBehaviourSchemaType::Number },
			{ "label", core::AgentBehaviourSchemaType::String },
			{ "delay", core::AgentBehaviourSchemaType::Duration },
			{ "destination", core::AgentBehaviourSchemaType::Marker },
			{ "route", core::AgentBehaviourSchemaType::List,
				{ { "item", core::AgentBehaviourSchemaType::Marker } } }
		};
	}

	core::AgentBehaviourConfiguration configuration(World const& world)
	{
		return {
			{ "enabled", true }, { "count", int64_t{ 7 } }, { "weight", 2.5 },
			{ "label", std::string("night") },
			{ "delay", core::AgentBehaviourDuration{ 12 } },
			{ "destination", world.alpha },
			{ "route", core::AgentBehaviourConfigurationList{
				world.alpha, world.beta } }
		};
	}

	AgentClipboardPayload parse(std::string const& text)
	{
		AgentClipboardPayload payload;
		std::string diagnostic;
		require(readAgentClipboardObject(
			YAML::Load(text)["prometheumClipboard"]["object"], payload, diagnostic),
			"Clipboard parse failed: " + diagnostic);
		return payload;
	}

	void clipboardPreservesAndResolvesDeliberately()
	{
		auto registry = core::AgentBehaviourRegistry::create();
		auto behaviour = registry->addAgentBehaviour("Schedule", "schedule.lua", schema());
		auto source = makeWorld("Source", registry, false);
		auto agent = source.world->createAgent("Ada", source.room, 0, 2.0f);
		std::string diagnostic;
		require(source.world->setAgentBehaviourAssignment(agent, behaviour, 1,
			configuration(source), &diagnostic), diagnostic);
		auto text = makeAgentClipboardText(makeAgentClipboardPayload(
			*source.world, agent, "Ada copy"), false);
		require(text.find("registryUuid") != std::string::npos
			&& text.find("identity") != std::string::npos
			&& text.find("type: marker") != std::string::npos
			&& text.find("value: Alpha") != std::string::npos,
			"Clipboard text omitted portable behaviour or Marker identity");
		auto payload = parse(text);

		gWorldDocumentHistory.clear();
		core::AgentId same{};
		require(commitAgentPlacement(source.world, payload,
			source.world->getSector(source.room), 0, 3.0f, same, diagnostic), diagnostic);
		require(source.world->getAgentBehaviourAssignment(same)
			== source.world->getAgentBehaviourAssignment(agent),
			"Same-World paste changed typed behaviour configuration");

		auto destination = makeWorld("Destination", registry, true);
		require(destination.alpha != source.alpha,
			"Cross-World fixture did not use foreign Marker IDs");
		core::AgentId crossed{};
		require(commitAgentPlacement(destination.world, payload,
			destination.world->getSector(destination.room), 0, 3.0f,
			crossed, diagnostic), diagnostic);
		auto assignment = destination.world->getAgentBehaviourAssignment(crossed);
		require(assignment && *core::agentBehaviourConfigurationGetIf<core::MarkerId>(
			&assignment->configuration.at("destination")) == destination.alpha,
			"Cross-World paste retained a foreign Marker ID");

		PendingAgentPlacement pending;
		require(armAgentPlacement(pending, *destination.world, payload,
			destination.world->getSector(destination.room), 0, 4.0f, diagnostic),
			diagnostic);
		auto before = destination.world->getSimulationSnapshot().agents.size();
		pending.cancel();
		require(destination.world->getSimulationSnapshot().agents.size() == before,
			"Cancelling a behaviour paste changed the World");

		auto missing = std::make_shared<core::World>("Missing", 12, 2);
		auto room = missing->addRoom("Room", 0, 0, 0, 12, 1);
		missing->addSectorMarker(room, 0, 8.5f, "Alpha");
		missing->finishBuild();
		missing->pauseSimulation();
		auto otherRegistry = core::AgentBehaviourRegistry::create();
		missing->attachAgentBehaviourRegistry("other.behaviours", otherRegistry);
		require(!armAgentPlacement(pending, *missing, payload,
			missing->getSector(room), 0, 3.0f, diagnostic)
			&& diagnostic.find("identity mismatch") != std::string::npos
			&& diagnostic.find("Beta") != std::string::npos
			&& missing->getSimulationSnapshot().agents.empty(),
			"Dependency refusal was not atomic or diagnostically complete");

		auto malformed = YAML::Load(
			"name: Ada\nflags: 0\nbehaviour:\n"
			"  registryUuid: not-a-uuid\n  identity: 1\n  revision: 1\n"
			"  configuration: []\n");
		AgentClipboardPayload invalid;
		require(!readAgentClipboardObject(malformed, invalid, diagnostic),
			"Malformed behaviour clipboard payload was accepted");
	}

	void saveAsCopiesWholePackageAndRollsBackFailures()
	{
		TemporaryDirectory temporary;
		auto sourceDirectory = temporary.path / "source";
		std::filesystem::create_directory(sourceDirectory);
		auto worldPath = sourceDirectory / "station.world.yaml";
		auto world = std::make_shared<core::World>("Station", 10, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 10, 1);
		world->addSectorMarker(room, 0, 7.5f, "Alpha");
		world->finishBuild();
		world->pauseSimulation();
		world->saveTo(worldPath.string());
		auto registry = core::createAndAttachAgentBehaviourRegistry(*world, worldPath);
		auto package = core::defaultAgentBehaviourRegistryPackagePath(worldPath);
		writeFile(package / "modules" / "schedule.lua",
			"return { api_version = 1, factory = function(configuration) return {} end }\n");
		auto behaviour = registry->addAgentBehaviour("Schedule",
			"modules/schedule.lua", { { "destination", core::AgentBehaviourSchemaType::Marker } });
		world->pauseSimulation();
		auto agent = world->createAgent("Ada", room, 0, 2.0f);
		std::string diagnostic;
		require(world->setAgentBehaviourAssignment(agent, behaviour, 1,
			{ { "destination", world->getMarkerIds().front() } }, &diagnostic), diagnostic);
		DocumentHistory history;
		require(saveWorldDocument({ world, worldPath.string(), {}, &history,
			package.string() }, &diagnostic), diagnostic);
		auto sourceUuid = registry->getUuid();

		auto destinationDirectory = temporary.path / "copy";
		std::filesystem::create_directory(destinationDirectory);
		auto destinationWorld = destinationDirectory / "copy.world.yaml";
		require(saveWorldDocument({ world, destinationWorld.string(), {},
			&history, package.string() }, &diagnostic),
			"Behaviour package Save As failed: " + diagnostic);
		auto copiedPackage = destinationDirectory / package.filename();
		require(std::filesystem::is_regular_file(copiedPackage / "behaviours.yaml")
			&& std::filesystem::is_regular_file(copiedPackage / "modules" / "schedule.lua")
			&& world->getExpectedAgentBehaviourRegistryUuid() != sourceUuid,
			"Save As did not install an independent complete behaviour package");
		auto copiedRegistry = world->getAgentBehaviourRegistry();
		world.reset();
		require(core::unloadAgentBehaviourRegistryDocumentIfUnused(copiedRegistry),
			"Copied package remained manager-owned after its World closed");
		copiedRegistry.reset();
		auto reopened = core::loadWorldDocument(destinationWorld);
		require(reopened->hasAttachedAgentBehaviourRegistry()
			&& reopened->getAgentBehaviourAssignmentCount() == 1,
			"Copied World/package set did not reload from disk with its assignment");

		auto failureDirectory = temporary.path / "failure";
		std::filesystem::create_directory(failureDirectory);
		auto failedWorld = failureDirectory / "failed.world.yaml";
		core::YamlSerializer::setWriteFailureAfterBytesForTesting(1);
		auto saved = saveWorldDocument({ reopened, failedWorld.string(), {},
			&history, copiedPackage.string() }, &diagnostic);
		core::YamlSerializer::setWriteFailureAfterBytesForTesting(0);
		require(!saved && !std::filesystem::exists(
			failureDirectory / copiedPackage.filename())
			&& reopened->getExpectedAgentBehaviourRegistryUuid()
				!= sourceUuid,
			"Failed Save As left a package or changed the source dependency");
	}
}

void runAgentBehaviourPortabilitySmokeChecks()
{
	clipboardPreservesAndResolvesDeliberately();
	saveAsCopiesWholePackageAndRollsBackFailures();
}
