// Shared Agent behaviour schema reconciliation, ticket #160.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "BehavioursPanel.h"
#include "DocumentEdit.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"

void runAgentBehaviourSchemaReconciliationSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path = std::filesystem::temp_directory_path()
			/ ("promethium-fermide-behaviour-schema-"
				+ std::to_string(std::chrono::steady_clock::now()
					.time_since_epoch().count()));
		TemporaryDirectory() { std::filesystem::create_directories(path); }
		~TemporaryDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	void writeText(std::filesystem::path const& path, std::string const& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output) throw std::runtime_error("Could not write schema fixture");
	}

	YAML::Node readYaml(std::filesystem::path const& path)
	{
		return YAML::LoadFile(path.string());
	}

	void writeYaml(std::filesystem::path const& path, YAML::Node const& node)
	{
		writeText(path, YAML::Dump(node));
	}

	YAML::Node scheduleSchema(bool optionalLabel, bool requiredPriority)
	{
		YAML::Node schema(YAML::NodeType::Sequence);
		YAML::Node schedule;
		schedule["name"] = "schedule";
		schedule["type"] = "list";
		YAML::Node element;
		element["name"] = "entry";
		element["type"] = "record";
		YAML::Node fields(YAML::NodeType::Sequence);
		YAML::Node destination;
		destination["name"] = "destination";
		destination["type"] = "marker";
		fields.push_back(destination);
		if (optionalLabel)
		{
			YAML::Node label;
			label["name"] = "label";
			label["type"] = "string";
			label["required"] = false;
			label["default"] = "stop";
			fields.push_back(label);
		}
		if (requiredPriority)
		{
			YAML::Node priority;
			priority["name"] = "priority";
			priority["type"] = "integer";
			fields.push_back(priority);
		}
		element["children"] = fields;
		YAML::Node children(YAML::NodeType::Sequence);
		children.push_back(element);
		schedule["children"] = children;
		schema.push_back(schedule);
		return schema;
	}

	core::AgentBehaviourConfiguration configuration(core::MarkerId marker,
		bool label)
	{
		core::AgentBehaviourConfigurationRecord entry{
			{ "destination", marker }
		};
		if (label) entry["label"] = std::string("authored");
		return { { "schedule", core::AgentBehaviourConfigurationList{ entry } } };
	}

	void reconcilesAndMigratesAcrossLoadedWorlds()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "shared.behaviours";
		auto const manifest = package / "behaviours.yaml";
		std::filesystem::create_directories(package);
		writeText(package / "schedule.lua",
			"return { api_version = 1, factory = function(configuration) return {} end }\n");
		auto const uuid = std::string("123e4567-e89b-42d3-a456-426614174160");
		YAML::Node root;
		root["version"] = 1;
		root["uuid"] = uuid;
		root["revision"] = 1;
		root["nextBehaviourId"] = 2;
		YAML::Node definition;
		definition["id"] = 1;
		definition["name"] = "Schedule";
		definition["revision"] = 1;
		definition["source"] = "schedule.lua";
		definition["schema"] = scheduleSchema(false, false);
		root["behaviours"].push_back(definition);
		writeYaml(manifest, root);

		struct Participant
		{
			std::shared_ptr<core::World> world;
			std::filesystem::path path;
			core::AgentId agent{};
			core::MarkerId marker{};
		};
		auto makeParticipant = [&](std::string name, std::string filename)
		{
			Participant participant;
			participant.world = std::make_shared<core::World>(name, 8, 2);
			auto room = participant.world->addRoom("Room", 0, 0, 0, 8, 1);
			participant.world->addSectorMarker(room, 0, 6.5f, "Destination");
			participant.marker = participant.world->getMarkerIds().front();
			participant.agent = participant.world->createAgent(
				name + " Agent", room, 0, 1.5f);
			participant.world->finishBuild();
			participant.world->pauseSimulation();
			participant.path = temporary.path / filename;
			participant.world->saveTo(participant.path.string());
			return participant;
		};
		auto first = makeParticipant("Alpha", "alpha.world.yaml");
		auto second = makeParticipant("Beta", "beta.world.yaml");
		auto registry = core::selectAndAttachAgentBehaviourRegistry(
			*first.world, first.path, package);
		require(core::selectAndAttachAgentBehaviourRegistry(
			*second.world, second.path, package) == registry,
			"Schema fixtures did not share one registry");
		for (auto* participant : { &first, &second })
		{
			require(participant->world->setAgentBehaviourAssignment(
				participant->agent, core::AgentBehaviourId{ 1 }, 1,
				configuration(participant->marker, false)),
				"Could not assign schema fixture behaviour");
			participant->world->saveTo(participant->path.string());
		}
		gWorldDocumentHistory.clear();
		auto const worldHistory = gWorldDocumentHistory.currentStateId();

		// A nested optional field with a valid default is classified for every
		// dependent configuration and materialized in stable World/Agent order.
		root = readYaml(manifest);
		root["behaviours"][0]["revision"] = 2;
		root["behaviours"][0]["schema"]
			= scheduleSchema(true, false);
		writeYaml(manifest, root);
		core::AgentBehaviourSchemaMigrationPreview preview;
		std::string diagnostic;
		require(core::previewAgentBehaviourRegistrySchemaMigration(
			registry, package, preview, &diagnostic), diagnostic);
		require(!preview.requiresExplicitMigration
			&& preview.configurations.size() == 2
			&& preview.configurations[0].worldName == "Alpha"
			&& preview.configurations[1].worldName == "Beta"
			&& preview.configurations[0].fields.size() == 1
			&& preview.configurations[0].fields[0].path == "schedule[].label",
			"Compatible preview omitted a World, Agent, or nested field");
		require(core::reloadAgentBehaviourRegistryDocument(
			registry, package, &diagnostic), diagnostic);
		for (auto* participant : { &first, &second })
		{
			auto const& assignment = *participant->world
				->getAgentBehaviourAssignment(participant->agent);
			auto const* list = core::agentBehaviourConfigurationGetIf<
				core::AgentBehaviourConfigurationList>(
					&assignment.configuration.at("schedule"));
			auto const* record = list ? core::agentBehaviourConfigurationGetIf<
				core::AgentBehaviourConfigurationRecord>(&list->front()) : nullptr;
			require(assignment.revision == 2 && record
				&& record->contains("label") && participant->world->isModified(),
				"Compatible reconciliation did not update revision/default atomically");
		}
		require(registry->isModified()
			&& gWorldDocumentHistory.currentStateId() == worldHistory,
			"Coordinated reconciliation mixed registry and World histories");
		require(saveAgentBehaviourRegistry(registry, package.string(), &diagnostic),
			diagnostic);

		// Removing the nested field is incompatible. Missing even one explicit
		// configuration refuses the whole operation without partial mutation.
		root = readYaml(manifest);
		root["behaviours"][0]["revision"] = 3;
		root["behaviours"][0]["schema"]
			= scheduleSchema(false, false);
		writeYaml(manifest, root);
		preview = {};
		require(core::previewAgentBehaviourRegistrySchemaMigration(
			registry, package, preview, &diagnostic)
			&& preview.requiresExplicitMigration
			&& preview.configurations.size() == 2
			&& preview.configurations[0].fields[0].path == "schedule[].label",
			"Incompatible nested removal was not fully previewed");
		std::vector<core::AgentBehaviourConfigurationMigration> migrations{
			{ first.world.get(), first.agent,
				configuration(first.marker, false) }
		};
		require(!core::migrateAgentBehaviourRegistryDocument(
			registry, package, migrations, &diagnostic)
			&& registry->lookupAgentBehaviour(core::AgentBehaviourId{ 1 })
				->getRevision() == 2
			&& first.world->getAgentBehaviourAssignment(first.agent)->revision == 2
			&& second.world->getAgentBehaviourAssignment(second.agent)->revision == 2,
			"Incomplete coordinated migration partially changed a participant");
		migrations.push_back({ second.world.get(), second.agent,
			configuration(second.marker, false) });
		require(core::migrateAgentBehaviourRegistryDocument(
			registry, package, migrations, &diagnostic), diagnostic);
		require(first.world->getAgentBehaviourAssignment(first.agent)->revision == 3
			&& second.world->getAgentBehaviourAssignment(second.agent)->revision == 3,
			"Explicit migration did not commit every participant");
		require(saveAgentBehaviourRegistry(registry, package.string(), &diagnostic),
			diagnostic);
		first.world->saveTo(first.path.string());
		second.world->saveTo(second.path.string());

		// A closed World opened against an incompatible newer revision keeps
		// its authored values and reports a recoverable, simulation-blocking state.
		second.world.reset();
		first.world.reset();
		require(core::unloadAgentBehaviourRegistryDocumentIfUnused(registry, true),
			"Could not release the schema registry before closed-World evolution");
		registry.reset();
		root = readYaml(manifest);
		YAML::Node revisionThree;
		revisionThree["revision"] = 3;
		revisionThree["schema"] = scheduleSchema(false, false);
		root["behaviours"][0]["schemaHistory"].push_back(revisionThree);
		root["behaviours"][0]["revision"] = 4;
		root["behaviours"][0]["schema"]
			= scheduleSchema(false, true);
		writeYaml(manifest, root);
		auto reopened = core::loadWorldDocument(temporary.path / "alpha.world.yaml");
		auto const& preserved = *reopened->getAgentBehaviourAssignment(first.agent);
		require(preserved.revision == 3,
			"Incompatible open changed the authored assignment revision");
		require(!reopened->agentBehaviourConfigurationsAreValid(),
			"Incompatible open reported valid configuration");
		require(reopened->getAgentBehaviourDependencyDiagnostic().find(
			"schedule[].priority") != std::string::npos,
			"Incompatible open omitted the nested mismatch: "
				+ reopened->getAgentBehaviourDependencyDiagnostic());
		require(!reopened->resumeSimulation(),
			"Incompatible open allowed simulation");
		bool saveRefused = !saveAgentBehaviourRegistry(
			reopened->getAgentBehaviourRegistry(), package.string(), &diagnostic)
			&& diagnostic.find("invalid dependent") != std::string::npos;
		require(saveRefused, "Registry save accepted an invalid dependent configuration");
	}
}

void runAgentBehaviourSchemaReconciliationSmokeChecks()
{
	reconcilesAndMigratesAcrossLoadedWorlds();
}
