// End-to-end deterministic Agent behaviour workflow verification for #164.

#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "AgentBehaviourAssignmentPanel.h"
#include "BehavioursPanel.h"
#include "DocumentEdit.h"
#include "MarkerPanel.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/World.h"
#include "core/Log.h"
#include "core/MarkerSectorObject.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui.h"

void runAgentBehaviourWorkflowSmokeChecks();

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
				/ ("prometheum-fermide-workflow-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}

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
		output.write(text.data(), static_cast<std::streamsize>(text.size()));
		if (!output) throw std::runtime_error("Could not write workflow fixture");
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

	std::shared_ptr<core::World> deserializeWorld(std::string const& yaml,
		std::shared_ptr<core::AgentBehaviourRegistry> const& registry = {})
	{
		auto world = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(world->deserialize(*reader, work),
			"A workflow World document did not deserialize");
		if (registry && world->hasAgentBehaviourRegistryReference())
			world->resolveAgentBehaviourRegistry(registry);
		return world;
	}

	std::vector<core::AgentBehaviourSchemaField> scheduleSchema()
	{
		std::vector<core::AgentBehaviourSchemaField> entry{
			{ "duration", core::AgentBehaviourSchemaType::Duration },
			{ "destination", core::AgentBehaviourSchemaType::Marker }
		};
		return {
			{ "code", core::AgentBehaviourSchemaType::String },
			{ "start_delay", core::AgentBehaviourSchemaType::Duration },
			{ "fallback", core::AgentBehaviourSchemaType::Marker },
			{ "fail_on_interaction", core::AgentBehaviourSchemaType::Boolean },
			{ "schedule", core::AgentBehaviourSchemaType::List, {
				{ "entry", core::AgentBehaviourSchemaType::Record, std::move(entry) }
			} }
		};
	}

	std::string const ScheduleSource = R"lua(
local host = require("prometheum.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local index = 1
    local function callback(context, value)
      context.log("CB:" .. configuration.code .. ":" .. value .. ":" .. context.tick)
    end
    local function move(context, destination, label)
      local result = context.move_to(destination)
      context.log("CMD:" .. configuration.code .. ":" .. label .. ":" .. result.status .. ":" .. context.tick)
      if not result.accepted then error("move " .. label .. ":" .. result.status) end
    end
    return {
      on_start = function(context)
        callback(context, "start")
        context.set_timer("depart", configuration.start_delay)
      end,
      on_timer = function(name, context)
        callback(context, "timer-" .. name)
        if name == "depart" then
          move(context, configuration.schedule[index].destination, "depart")
        elseif name == "cancel" then
          local result = context.cancel_movement()
          context.log("CMD:" .. configuration.code .. ":cancel:" .. result.status .. ":" .. context.tick)
          if not result.accepted then error("cancel:" .. result.status) end
        else
          error("unexpected timer " .. name)
        end
      end,
      on_event = function(event, context)
        callback(context, "event-" .. event.type)
        if event.type == "destination_reached" and index < 2 then
          index = index + 1
          move(context, configuration.schedule[index].destination, "next")
          context.set_timer("cancel", configuration.schedule[index].duration)
        elseif event.type == "movement_cancelled" then
          move(context, configuration.fallback, "after-cancel")
        elseif event.type == "interaction_failed" and configuration.fail_on_interaction then
          error("expected interaction failure for " .. configuration.code)
        end
      end,
      on_route_lost = function(destination, reason, context)
        callback(context, "route-" .. reason)
        move(context, configuration.fallback, "route-fallback")
      end,
      on_stop = function(reason, context)
        context.log("CB:" .. configuration.code .. ":stop-" .. reason .. ":" .. context.tick)
      end
    }
  end
}
)lua";

	core::AgentBehaviourConfiguration scheduleConfiguration(std::string code,
		uint64_t delay, core::MarkerId fallback, bool failOnInteraction,
		std::vector<std::pair<core::MarkerId, uint64_t>> const& entries)
	{
		core::AgentBehaviourConfigurationList schedule;
		for (auto const& [destination, duration] : entries)
			schedule.emplace_back(core::AgentBehaviourConfigurationRecord{
				{ "duration", core::AgentBehaviourDuration{ duration } },
				{ "destination", destination }
			});
		return {
			{ "code", std::move(code) },
			{ "start_delay", core::AgentBehaviourDuration{ delay } },
			{ "fallback", fallback },
			{ "fail_on_interaction", failOnInteraction },
			{ "schedule", std::move(schedule) }
		};
	}

	struct WorkflowDigest
	{
		std::string snapshots;
		std::string events;
		std::string diagnostics;
		std::string callbacks;
		std::string commands;

		bool operator==(WorkflowDigest const&) const = default;
	};

	struct WorkflowObservations
	{
		bool deactivated{ false };
		bool activated{ false };
		bool routeLost{ false };
		bool cancelled{ false };
		bool reached{ false };
		bool interactionCompleted{ false };
		bool interactionFailed{ false };
	};

	void appendSnapshot(std::ostringstream& output,
		core::SimulationSnapshot const& snapshot)
	{
		output << snapshot.tick << ':' << snapshot.paused << ':'
			<< snapshot.topologyGeneration << '[';
		for (auto const& agent : snapshot.agents)
			output << agent.id.value << ',' << agent.sectorId.value << ','
				<< std::bit_cast<uint32_t>(agent.globalPosition.x) << ','
				<< std::bit_cast<uint32_t>(agent.globalPosition.y) << ','
				<< static_cast<unsigned>(agent.state) << ',' << agent.active << ','
				<< agent.hasPath << ',' << agent.targetPathNode << ','
				<< agent.pathNodeCount << ';';
		output << "]{";
		for (auto const& request : snapshot.interactionRequests)
			output << request.id.value << ',' << request.actor.value << ','
				<< static_cast<unsigned>(request.result) << ';';
		output << "}|";
	}

	void appendEvents(std::ostringstream& output,
		std::vector<core::SimulationEvent> const& events,
		WorkflowObservations& observed)
	{
		for (auto const& event : events)
		{
			bool semantic = false;
			switch (event.type)
			{
			case core::SimulationEventType::DestinationReached:
				observed.reached = semantic = true; break;
			case core::SimulationEventType::MovementCancelled:
				observed.cancelled = semantic = true; break;
			case core::SimulationEventType::RouteLost:
				observed.routeLost = semantic = true; break;
			case core::SimulationEventType::AgentActivated:
				observed.activated = semantic = true; break;
			case core::SimulationEventType::AgentDeactivated:
				observed.deactivated = semantic = true; break;
			case core::SimulationEventType::InteractionRequestChanged:
				if (event.interactionRequest.result != core::InteractionResult::Pending)
				{
					semantic = true;
					if (event.interactionRequest.result == core::InteractionResult::Succeeded
						|| event.interactionRequest.result
							== core::InteractionResult::SucceededWithBestEffortFailure)
						observed.interactionCompleted = true;
					else observed.interactionFailed = true;
				}
				break;
			default: break;
			}
			if (!semantic) continue;
			output << event.sequence << ':' << event.tick << ':'
				<< static_cast<unsigned>(event.type) << ':' << event.agent.id.value
				<< ':' << event.destinationMarker.value << ':'
				<< static_cast<unsigned>(event.routeLossReason) << ':'
				<< event.interactionRequest.id.value << ':'
				<< static_cast<unsigned>(event.interactionRequest.result) << '|';
		}
	}

	void appendDiagnostics(std::ostringstream& output,
		std::vector<core::AgentBehaviourRuntimeDiagnostic> const& diagnostics)
	{
		for (auto const& item : diagnostics)
			output << static_cast<unsigned>(item.failure) << ':'
				<< static_cast<unsigned>(item.stage) << ':' << item.agent.value << ':'
				<< item.behaviour.value << ':' << item.tick << ':' << item.callback
				<< ':' << item.diagnostic << ':' << item.traceback << '|';
	}

	void appendLogs(std::ostringstream& callbacks, std::ostringstream& commands)
	{
		for (auto const& message : core::consumeLogMessages())
		{
			if (message.msg.starts_with("CB:")) callbacks << message.msg << '|';
			if (message.msg.starts_with("CMD:")) commands << message.msg << '|';
		}
	}

	WorkflowDigest runCompleteWorkflow()
	{
		(void)core::consumeLogMessages();
		TemporaryDirectory temporary;
		auto const package = temporary.path / "shared.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "schedule.lua", ScheduleSource);
		auto const behaviour = registry->addAgentBehaviour(
			"Shared schedule", "schedule.lua", scheduleSchema());
		registry->saveTo((package / "behaviours.yaml").string());

		auto world = std::make_shared<core::World>("Complete workflow", 40, 2);
		auto const mainRoom = world->addRoom("Main", 0, 0, 0, 40, 1);
		auto const isolatedRoom = world->addRoom("Isolated", 1, 0, 0, 10, 1);
		world->addSectorMarker(mainRoom, 0, 3.5f, "Fallback");
		world->addSectorMarker(mainRoom, 0, 8.5f, "Work");
		world->addSectorMarker(mainRoom, 0, 18.5f, "Lunch");
		world->addSectorMarker(mainRoom, 0, 30.5f, "Home");
		world->addSectorMarker(isolatedRoom, 0, 4.5f, "Isolated destination");
		auto const sector = core::SectorId{ static_cast<uint64_t>(mainRoom) + 1 };
		core::InteractionBinding binding{
			{ core::DeviceCommandType::SetSectorLights, sector, true },
			core::InteractionBindingRequirement::Required };
		auto const working = world->createInteractionPoint("Working control", sector,
			{ 0.5f, 0.0f }, 100.0f, 0.0f, { binding });
		auto const broken = world->createInteractionPoint("Broken control", sector,
			{ 0.5f, 0.0f }, 100.0f, 0.0f, { binding });
		world->finishBuild();
		auto const first = world->createAgent("First", mainRoom, 0, 0.5f);
		auto const second = world->createAgent("Second", mainRoom, 0, 1.5f);
		auto const third = world->createAgent("Third", mainRoom, 0, 2.5f);
		world->pauseSimulation();
		world->attachAgentBehaviourRegistry("shared.behaviours", registry);
		auto const markers = world->getMarkerIds();
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		std::string diagnostic;
		require(world->setAgentBehaviourAssignment(first, behaviour, revision,
			scheduleConfiguration("A", 3, markers[0], false,
				{ { markers[1], 1 }, { markers[3], 3 } }), &diagnostic)
			&& world->setAgentBehaviourAssignment(second, behaviour, revision,
				scheduleConfiguration("B", 1, markers[0], false,
					{ { markers[4], 1 }, { markers[2], 2 } }), &diagnostic)
			&& world->setAgentBehaviourAssignment(third, behaviour, revision,
				scheduleConfiguration("C", 1, markers[0], true,
					{ { markers[2], 1 }, { markers[3], 2 } }), &diagnostic),
			"Could not assign the independently configured shared schedules: " + diagnostic);
		require(world->resumeSimulation(), "Could not start the complete workflow");
		world->consumeSimulationEvents();

		WorkflowObservations observed;
		std::ostringstream snapshots, events, diagnostics, callbacks, commands;
		auto advance = [&]
		{
			auto const advanced = world->advanceTick();
			appendSnapshot(snapshots, world->getSimulationSnapshot());
			appendEvents(events, world->consumeSimulationEvents(), observed);
			appendDiagnostics(diagnostics,
				world->consumeAgentBehaviourRuntimeDiagnostics());
			appendLogs(callbacks, commands);
			return advanced;
		};

		require(advance(), "The startup boundary failed");
		world->pauseSimulation();
		appendEvents(events, world->consumeSimulationEvents(), observed);
		require(world->setAgentActive(first, false),
			"Could not deactivate the timer-driven Agent");
		appendEvents(events, world->consumeSimulationEvents(), observed);
		require(world->resumeSimulation(), "Could not resume with a suspended Agent");
		world->consumeSimulationEvents();
		for (unsigned tick = 0; tick < 4; ++tick)
			require(advance(), "A suspended timer stopped the workflow");
		world->pauseSimulation();
		world->consumeSimulationEvents();
		require(world->setAgentActive(first, true),
			"Could not reactivate the timer-driven Agent");
		appendEvents(events, world->consumeSimulationEvents(), observed);
		require(world->resumeSimulation(), "Could not resume the reactivated Agent");
		world->consumeSimulationEvents();

		for (unsigned tick = 0; tick < 2400
			&& (!observed.routeLost || !observed.cancelled || !observed.reached); ++tick)
		{
			if (tick == 12)
			{
				world->pauseSimulation();
				require(world->resumeSimulation(),
					"Ordinary pause/resume did not preserve the schedules");
				world->consumeSimulationEvents();
			}
			auto const advanced = advance();
			require(advanced, "A healthy schedule boundary stopped unexpectedly at loop "
				+ std::to_string(tick) + ", simulation tick "
				+ std::to_string(world->getSimulationTick()) + ", paused="
				+ std::to_string(world->isSimulationPaused()) + ": diagnostics="
				+ diagnostics.str());
		}
		require(observed.deactivated && observed.activated && observed.routeLost
			&& observed.cancelled && observed.reached,
			"The shared schedules did not cover activation, route loss, cancellation, and destinations");
		// Require several idle boundaries so a just-published destination event
		// has time to launch its configured second leg before we call it settled.
		unsigned idleBoundaries = 0;
		for (unsigned tick = 0; tick < 10'000 && idleBoundaries < 3; ++tick)
		{
			bool allIdle = true;
			for (auto const& item : world->getSimulationSnapshot().agents)
				allIdle = allIdle && !item.hasPath
					&& item.state == core::AgentPathState::Idle;
			idleBoundaries = allIdle ? idleBoundaries + 1 : 0;
			if (idleBoundaries < 3)
				require(advance(), "The schedules did not settle before interactions");
		}
		for (auto const& item : world->getSimulationSnapshot().agents)
			require(!item.hasPath && item.state == core::AgentPathState::Idle,
				"Scheduled Agent " + std::to_string(item.id.value)
					+ " was still moving when interaction coverage began (state "
					+ std::to_string(static_cast<unsigned>(item.state)) + ", x "
					+ std::to_string(item.globalPosition.x) + ", hasPath "
					+ std::to_string(item.hasPath) + ")");

		auto completeInteraction = [&](core::InteractionPointId point,
			core::AgentId agent, core::DeviceOperationState result)
		{
			auto const request = world->requestInteraction(point, agent);
			auto lookup = world->lookupInteractionRequest(request);
			auto actor = world->lookupAgent(agent);
			require(lookup && !lookup.entity->getOperations().empty(),
				"Could not create workflow interaction point "
					+ std::to_string(point.value) + " for Agent "
					+ std::to_string(agent.value) + " (request "
					+ std::to_string(request.value) + ", state "
					+ std::to_string(actor ? static_cast<unsigned>(actor.entity->getState()) : 99u)
					+ ", sector " + std::to_string(actor
						? actor.entity->getSector()->getIndex() : 999u) + ")");
			auto const operation = lookup.entity->getOperations().front().first;
			world->lookupDeviceOperation(operation).entity->setState(result);
		};
		completeInteraction(working, second, core::DeviceOperationState::Succeeded);
		require(advance() && advance(), "The successful interaction stopped the workflow");
		completeInteraction(broken, third, core::DeviceOperationState::Failed);
		require(advance(), "Publishing the failed interaction stopped too early");
		require(!advance() && world->isSimulationPaused(),
			"The expected callback diagnostic did not stop before the next tick");
		require(observed.interactionCompleted && observed.interactionFailed,
			"Public interaction outcomes were consumed or omitted by Lua");
		require(diagnostics.str().find("expected interaction failure for C")
				!= std::string::npos
			&& !world->agentBehaviourOwnsMovement(third)
			&& world->agentBehaviourOwnsMovement(first)
			&& world->agentBehaviourOwnsMovement(second),
			"The expected diagnostic lacked stable detail or failure isolation");
		require(world->resumeSimulation() && advance(),
			"The healthy instances could not resume after failure acknowledgement");

		world->pauseSimulation();
		world->resetSimulation();
		require(world->getSimulationTick() == 0
			&& world->getAgentBehaviourAssignmentCount() == 3,
			"Reset did not replace runtime state while retaining authored schedules");
		require(world->resumeSimulation() && advance(),
			"Reset schedules did not recreate their instances");
		require(callbacks.str().find("CB:C:start:0") != std::string::npos,
			"Reset did not recreate a previously failed instance");

		world->pauseSimulation();
		std::vector<core::AgentBehaviourReloadDiagnostic> reloadDiagnostics;
		require(core::reloadAgentBehaviourRegistryDocument(registry, package,
			&diagnostic, &reloadDiagnostics) && reloadDiagnostics.empty(),
			"An unchanged external source reload was not atomic: " + diagnostic);
		require(world->resumeSimulation() && advance(),
			"Reloaded schedules did not recreate and resume");

		require(callbacks.str().find("CB:A:event-deactivated") != std::string::npos
			&& callbacks.str().find("CB:A:event-activated") != std::string::npos
			&& callbacks.str().find("CB:B:route-unreachable") != std::string::npos
			&& commands.str().find("CMD:A:cancel:accepted") != std::string::npos
			&& commands.str().find("CMD:B:route-fallback:accepted") != std::string::npos,
			"Callback or command-order observations omitted a required workflow stage");

		return { snapshots.str(), events.str(), diagnostics.str(),
			callbacks.str(), commands.str() };
	}

	void completeWorkflowReplaysIdentically()
	{
		auto const first = runCompleteWorkflow();
		auto const second = runCompleteWorkflow();
		require(first == second,
			"Repeated fixed-tick workflow runs changed a snapshot, event, diagnostic, callback, or command digest");
	}

	void persistenceHistoryMigrationAndReplacementAreAtomic()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "documents.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "schedule.lua", ScheduleSource);
		auto const behaviour = registry->addAgentBehaviour(
			"Schedule", "schedule.lua", scheduleSchema());
		registry->saveTo((package / "behaviours.yaml").string());
		auto registryRoundTrip = core::AgentBehaviourRegistry::loadFrom(
			(package / "behaviours.yaml").string());
		require(registryRoundTrip->getUuid() == registry->getUuid()
			&& registryRoundTrip->getBehaviourCount() == 1,
			"The version-1 registry did not round-trip");

		auto world = std::make_shared<core::World>("Documents", 12, 2);
		auto const room = world->addRoom("Room", 0, 0, 0, 12, 1);
		world->addSectorMarker(room, 0, 2.5f, "Alpha");
		world->addSectorMarker(room, 0, 9.5f, "Beta");
		world->finishBuild();
		auto const agent = world->createAgent("Assigned", room, 0, 0.5f);
		world->pauseSimulation();
		world->attachAgentBehaviourRegistry("documents.behaviours", registry);
		auto const markers = world->getMarkerIds();
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		auto configuration = scheduleConfiguration("D", 1, markers[0], false,
			{ { markers[0], 1 }, { markers[1], 2 } });
		std::string diagnostic;
		require(world->setAgentBehaviourAssignment(agent, behaviour, revision,
			configuration, &diagnostic), "Could not author the document fixture");
		auto const currentYaml = serializeWorld(*world);
		require(currentYaml.find("version: 15") != std::string::npos,
			"The current World schema did not include the version-11 Marker data lineage");
		auto currentRoundTrip = deserializeWorld(currentYaml, registry);
		currentRoundTrip->pauseSimulation();
		require(currentRoundTrip->getAgentBehaviourAssignment(agent)
			== world->getAgentBehaviourAssignment(agent)
			&& currentRoundTrip->lookupMarker(markers[0])->getName() == "Alpha",
			"The complete World assignment and Marker state did not round-trip");

		// Version 11 is the Marker identity boundary. A document without later
		// behaviour fields still retains exact Marker identities and names.
		core::World markerOnly("Version 11", 8, 2);
		auto const markerRoom = markerOnly.addRoom("Room", 0, 0, 0, 8, 1);
		markerOnly.addSectorMarker(markerRoom, 0, 3.5f, "Named Marker");
		auto version11 = serializeWorld(markerOnly);
		auto versionOffset = version11.find("version: 15");
		require(versionOffset != std::string::npos, "Missing World version field");
		version11.replace(versionOffset, std::string("version: 15").size(), "version: 11");
		auto loaded11 = deserializeWorld(version11);
		require(loaded11->getMarkerIds().size() == 1
			&& loaded11->lookupMarker(loaded11->getMarkerIds().front())->getName()
				== "Named Marker",
			"A version-11 World did not retain named Marker identity");

		auto legacy = version11;
		versionOffset = legacy.find("version: 11");
		legacy.replace(versionOffset, std::string("version: 11").size(), "version: 10");
		auto migrated = deserializeWorld(legacy);
		require(migrated->getMarkerIds().front().value == 1
			&& migrated->lookupMarker(core::MarkerId{ 1 })->getName() == "Marker 1",
			"Legacy unnamed Marker migration was not deterministic");

		auto future = currentYaml;
		versionOffset = future.find("version: 15");
		future.replace(versionOffset, std::string("version: 15").size(), "version: 16");
		bool futureRefused = false;
		try { (void)deserializeWorld(future); }
		catch (std::exception const& error)
		{
			futureRefused = std::string(error.what()).find("Unsupported")
				!= std::string::npos;
		}
		require(futureRefused,
			"A reader outside its supported World version boundary did not refuse");

		auto malformed = version11;
		auto const nameOffset = malformed.find("name: Named Marker");
		require(nameOffset != std::string::npos, "Missing serialized Marker name");
		malformed.replace(nameOffset, std::string("name: Named Marker").size(),
			"name: ' '");
		auto current = currentRoundTrip;
		auto const beforeReplacement = serializeWorld(*current);
		auto replaceDocument = [&](std::string const& candidate)
		{
			try
			{
				auto parsed = deserializeWorld(candidate, registry);
				current = std::move(parsed);
				return true;
			}
			catch (...) { return false; }
		};
		require(!replaceDocument(malformed)
			&& serializeWorld(*current) == beforeReplacement,
			"A malformed whole-document replacement disturbed the open World");

		gWorldDocumentHistory.clear();
		auto edited = current->getAgentBehaviourAssignment(agent)->configuration;
		auto* code = core::agentBehaviourConfigurationGetIf<std::string>(
			&edited.at("code"));
		*code = "Edited";
		require(commitAgentBehaviourAssignment(current, agent, behaviour, revision,
			edited, diagnostic), "The assignment edit failed: " + diagnostic);
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			return replaceDocument(snapshot.yaml);
		};
		require(gWorldDocumentHistory.undo(
			gWorldDocumentHistory.capture(serializeWorld(*current)), restore)
			&& *core::agentBehaviourConfigurationGetIf<std::string>(
				&current->getAgentBehaviourAssignment(agent)->configuration.at("code")) == "D",
			"Undo did not replace the complete authored World");
		require(gWorldDocumentHistory.redo(
			gWorldDocumentHistory.capture(serializeWorld(*current)), restore)
			&& *core::agentBehaviourConfigurationGetIf<std::string>(
				&current->getAgentBehaviourAssignment(agent)->configuration.at("code"))
					== "Edited",
			"Redo did not replace the complete authored World");
		gWorldDocumentHistory.clear();

		writeText(package / "malformed.yaml", "version: 1\nuuid: not-a-uuid\n");
		bool malformedRegistryRefused = false;
		try
		{
			(void)core::AgentBehaviourRegistry::loadFrom(
				(package / "malformed.yaml").string());
		}
		catch (...) { malformedRegistryRefused = true; }
		require(malformedRegistryRefused,
			"A malformed version-1 registry input was accepted");
	}

	void realPanelsRenderPausedRunningAndDiagnosticStates()
	{
		TemporaryDirectory temporary;
		auto const package = temporary.path / "panels.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "panel.lua", R"lua(
return { api_version = 1, factory = function()
  return { on_start = function() error("panel diagnostic") end }
end }
)lua");
		auto const behaviour = registry->addAgentBehaviour(
			"Panel schedule", "panel.lua", scheduleSchema());
		registry->saveTo((package / "behaviours.yaml").string());

		auto world = std::make_shared<core::World>("Panels", 12, 2);
		auto const room = world->addRoom("Room", 0, 0, 0, 12, 1);
		auto const markerObject = world->addSectorMarker(room, 0, 9.5f, "Destination");
		world->finishBuild();
		auto const agent = world->createAgent("Panel Agent", room, 0, 0.5f);
		world->pauseSimulation();
		auto const worldPath = temporary.path / "panels.world.yaml";
		world->saveTo(worldPath.string());
		world->attachAgentBehaviourRegistry("panels.behaviours", registry);
		auto const marker = world->getMarkerIds().front();
		std::string diagnostic;
		require(world->setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			scheduleConfiguration("Panel", 1, marker, false,
				{ { marker, 1 }, { marker, 1 } }), &diagnostic),
			"Could not assign the panel fixture: " + diagnostic);
		auto markerSelection = world->getSector(room)->getObject(markerObject.index);

		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(1024, 768);
		unsigned char* pixels = nullptr;
		int width = 0, height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		(void)pixels; (void)width; (void)height;
		auto renderFrame = [&]
		{
			ImGui::NewFrame();
			ImGui::Begin("Registry and behaviour");
			(void)renderBehavioursPanel(world, worldPath.string());
			ImGui::End();
			ImGui::Begin("Assignment, schedule, status, diagnostics");
			renderAgentBehaviourAssignmentCell(world, agent);
			renderAgentBehaviourConfigurationPanel(world, agent);
			ImGui::End();
			ImGui::Begin("Marker");
			renderMarkerEditorPanel(world, markerSelection);
			ImGui::End();
			ImGui::Render();
		};

		renderFrame(); // paused: editable nested schedule and named Marker controls
		require(world->resumeSimulation(), "Could not render the running panel state");
		renderFrame(); // running: assignment and schedule controls are disabled
		require(!world->advanceTick() && world->isSimulationPaused(),
			"The panel diagnostic fixture did not fail visibly");
		renderFrame(); // paused after failure: status, diagnostic, traceback, clear control
		require(world->getAgentBehaviourRuntimeDiagnostics().size() == 1,
			"Rendering diagnostics acknowledged them without the explicit Clear control");
		ImGui::DestroyContext();
		resetBehavioursPanelState();
	}
}

void runAgentBehaviourWorkflowSmokeChecks()
{
	completeWorkflowReplaysIdentically();
	persistenceHistoryMigrationAndReplacementAreAtomic();
	realPanelsRenderPausedRunningAndDiagnosticStates();
}
