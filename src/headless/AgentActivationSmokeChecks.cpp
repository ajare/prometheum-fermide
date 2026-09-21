// Agent activation and deactivation, for ticket #118.
//
// An activated Agent is simulated; a deactivated one keeps its authored
// position and route but no tick acts on it. Activation is authored state
// like the Agent group assignment: it crosses save/load, reset, topology
// replays and the clipboard, and the only place the simulation itself ever
// sees it is the point where a tick decides whose turn it is.
//
// Everything here drives the public Building API and a real tick pipeline;
// the registries behind the API are never inspected, and the YAML is read
// as a whole document - never asserted against incidental formatting.
//
// What gets pinned down:
//
//   every Agent starts activated, and so does every Agent loaded from a
//   document that predates the field
//   activating or deactivating is refused while the simulation runs - with
//   a reason, and changing nothing - and an unknown Agent is refused too
//   a deactivated Agent does not move or wake; the route a pause tore down
//   is not replayed onto it, and reactivating it later does not resurrect
//   that route
//   reactivating while the simulation is still paused puts the Agent back
//   under the simulation: its retained route replays and it walks
//   the flag crosses a whole-document save/load, a reset, an undoable
//   object-move replay, and the Agent clipboard
//   an Agent document that never carried the field loads activated, and a
//   clipboard payload that never carried it reads activated

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <yaml-cpp/yaml.h>

#include "core/Agent.h"
#include "core/Building.h"
#include "core/EntityId.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/YamlSerializer.h"

#include "AgentClipboard.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeBuilding(core::Building& building)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	// A whole-document load, the way the editor opens a file.
	std::shared_ptr<core::Building> loadBuilding(std::string const& yaml)
	{
		auto loaded = std::make_shared<core::Building>("Loaded Building", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(reader != nullptr, "The serialised Building could not be read back");
		require(loaded->deserialize(*reader, workData), "The Building did not reload");
		return loaded;
	}

	core::Agent const* findAgentByName(core::Building const& building, std::string const& name)
	{
		for (uint32_t layer = 0; layer < building.getLayerCount(); ++layer)
		{
			for (auto const& sector : building.getSectors(layer))
			{
				if (!sector) continue;
				for (auto* agent : sector->getAgents())
					if (agent && agent->getName() == name) return agent;
			}
		}
		return nullptr;
	}

	core::AgentSnapshot snapshotOf(core::Building const& building, core::AgentId id)
	{
		for (auto const& entry : building.getSimulationSnapshot().agents)
			if (entry.id == id) return entry;
		throw std::runtime_error("The snapshot does not name the Agent");
	}

	// One corridor, one marker at its far end, and two Agents at the near
	// end, each already walking toward the marker. The walk is what tells a
	// simulated Agent from a parked one.
	struct WalkFixture
	{
		core::Building building;
		uint32_t corridor;
		uint32_t destinationIdentifier;
		core::AgentId first;
		core::AgentId second;

		WalkFixture()
			: building("Activation walk", 12, 3)
			, destinationIdentifier{ 0x41475231u }
		{
			corridor = building.addCorridor(0, 0, 8);
			building.addSectorMarker(corridor, 0, 7.5f, &destinationIdentifier);
			building.finishBuild();

			auto const destination = building.getGraph()->getVertexByIdentifier(destinationIdentifier);
			require(destination != nullptr, "The walk destination vertex is missing");

			first = building.createAgent("Walker", corridor, 0, 0.5f);
			second = building.createAgent("Parker", corridor, 0, 1.5f);
			for (auto id : { first, second })
			{
				auto* agent = building.lookupAgent(id).entity;
				auto path = building.getGraph()->calculatePath(agent, destination);
				require(path && !path->nodes.empty(), "The walk route could not be calculated");
				agent->setPath(std::move(path), true);
			}
		}
	};

	void everyAgentStartsActivated()
	{
		core::Building building("Fresh Building", 4, 1);
		auto const corridor = building.addCorridor(0, 0, 4);
		building.finishBuild();
		auto const id = building.createAgent("Alice", corridor);
		auto* agent = building.lookupAgent(id).entity;
		require(agent->isActive(), "A newly created Agent has to start activated");
		require(snapshotOf(building, id).active, "A newly created Agent's snapshot has to say activated");
	}

	void activationIsRefusedWhileTheSimulationRuns()
	{
		WalkFixture fixture;
		std::string diagnostic;

		// A fresh Building simulates from the first tick, so this run is live.
		require(!fixture.building.isSimulationPaused(), "The fixture has to start running");
		require(!fixture.building.canSetAgentActive(fixture.second, false, &diagnostic),
			"canSetAgentActive agreed to a deactivation while the simulation was running");
		require(!diagnostic.empty(), "A refused deactivation gave no reason");
		require(!fixture.building.setAgentActive(fixture.second, false, &diagnostic),
			"setAgentActive deactivated an Agent while the simulation was running");
		require(fixture.building.lookupAgent(fixture.second).entity->isActive(),
			"A refused deactivation changed the Agent anyway");
		require(snapshotOf(fixture.building, fixture.second).active,
			"A refused deactivation reached the snapshot anyway");

		require(!fixture.building.canSetAgentActive(core::AgentId{}, false, &diagnostic),
			"canSetAgentActive accepted an Agent the Building does not own");
		require(!diagnostic.empty(), "An unknown Agent gave no reason for the refusal");
	}

	void deactivatedAgentsAreNotSimulated()
	{
		WalkFixture fixture;
		std::string diagnostic;

		// Let both Agents get underway, then park one. The parked position is
		// read after the pause: pausing snaps a mid-traversal Agent back to
		// the vertex its route began from, and it is the post-pause spot the
		// deactivated Agent has to keep.
		fixture.building.advanceTicks(20);
		fixture.building.pauseSimulation();
		require(fixture.building.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);
		auto const* parker = fixture.building.lookupAgent(fixture.second).entity;
		auto const parkedAt = parker->getGlobalPosition();
		require(fixture.building.resumeSimulation(), "The fixture could not resume");

		// The walk is ~7 units at 0.5 units a second of fixed 1/60 ticks, so
		// well over a thousand ticks are needed to actually finish it.
		for (uint32_t tick = 0; tick < 1500; ++tick) fixture.building.advanceTick();

		auto* walker = fixture.building.lookupAgent(fixture.first).entity;
		require(walker->getGlobalPosition().x > 6.0f,
			"The activated Agent did not finish its walk, so the comparison proved nothing");
		require(parker->getGlobalPosition() == parkedAt,
			"A deactivated Agent moved while the simulation ran");
		require(parker->getState() == core::Agent::State::Idle,
			"A deactivated Agent left the Idle state without being simulated");
		require(!parker->getPath(), "A deactivated Agent's torn-down route came back on its own");
		require(!snapshotOf(fixture.building, fixture.second).active,
			"The snapshot did not report the deactivation");
		require(snapshotOf(fixture.building, fixture.first).active,
			"The snapshot reported the walking Agent as deactivated");

		// Reactivating after the resume cannot resurrect the route the pause
		// tore down: the Agent stands where it was parked until the author
		// gives it a new one.
		fixture.building.pauseSimulation();
		require(fixture.building.setAgentActive(fixture.second, true, &diagnostic),
			"The reactivation was refused: " + diagnostic);
		require(fixture.building.resumeSimulation(), "The fixture could not resume again");
		for (uint32_t tick = 0; tick < 300; ++tick) fixture.building.advanceTick();
		require(parker->getGlobalPosition() == parkedAt,
			"Reactivation resurrected a route the pause had dropped");
	}

	void reactivationWhilePausedPutsTheAgentBackUnderTheSimulation()
	{
		WalkFixture fixture;
		std::string diagnostic;

		fixture.building.pauseSimulation();
		require(fixture.building.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);
		// Changed its mind before the resume: the pause's retained route must
		// replay like any other activated Agent's.
		require(fixture.building.setAgentActive(fixture.second, true, &diagnostic),
			"The reactivation was refused: " + diagnostic);
		require(fixture.building.resumeSimulation(), "The fixture could not resume");

		for (uint32_t tick = 0; tick < 1500; ++tick) fixture.building.advanceTick();
		require(fixture.building.lookupAgent(fixture.second).entity->getGlobalPosition().x > 6.0f,
			"An Agent reactivated while paused was not simulated on resume");
	}

	void wakingSkipsDeactivatedAgents()
	{
		WalkFixture fixture;
		std::string diagnostic;

		fixture.building.pauseSimulation();
		require(fixture.building.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);

		// Both Agents are Idle with their routes torn down. Hand each a route
		// without starting it, then wake the world: only the activated Agent
		// may answer.
		auto const destination = fixture.building.getGraph()->getVertexByIdentifier(
			fixture.destinationIdentifier);
		require(destination != nullptr, "The walk destination vertex is missing");
		for (auto id : { fixture.first, fixture.second })
		{
			auto* agent = fixture.building.lookupAgent(id).entity;
			auto path = fixture.building.getGraph()->calculatePath(agent, destination);
			require(path && !path->nodes.empty(), "The walk route could not be calculated");
			agent->setPath(std::move(path), false);
		}

		fixture.building.wakeAllAgents();
		require(fixture.building.lookupAgent(fixture.first).entity->getState()
			== core::Agent::State::MovingToVertex, "Waking did not start the activated Agent");
		require(fixture.building.lookupAgent(fixture.second).entity->getState()
			== core::Agent::State::Idle, "Waking started a deactivated Agent");

		require(fixture.building.setAgentActive(fixture.second, true, &diagnostic),
			"The reactivation was refused: " + diagnostic);
		fixture.building.wakeAllAgents();
		require(fixture.building.lookupAgent(fixture.second).entity->getState()
			== core::Agent::State::MovingToVertex, "Waking did not start the reactivated Agent");
	}

	void activationSurvivesSerializationAndReset()
	{
		WalkFixture fixture;
		std::string diagnostic;

		fixture.building.pauseSimulation();
		require(fixture.building.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);

		auto const yaml = serializeBuilding(fixture.building);
		require(yaml.find("active") != std::string::npos,
			"A deactivated Agent did not persist its activation at all");

		auto const loaded = loadBuilding(yaml);
		auto const* loadedParker = findAgentByName(*loaded, "Parker");
		require(loadedParker != nullptr, "The reloaded Building lost the parked Agent");
		require(!loadedParker->isActive(), "A whole-document load lost the deactivation");
		require(findAgentByName(*loaded, "Walker")->isActive(),
			"A whole-document load lost an activation");

		// Reset replays the authored document against the same Building: the
		// flag is authored state, so it must come through untouched.
		fixture.building.resetSimulation();
		require(!fixture.building.lookupAgent(fixture.second).entity->isActive(),
			"resetSimulation reactivated a deactivated Agent");
		require(fixture.building.lookupAgent(fixture.first).entity->isActive(),
			"resetSimulation deactivated an activated Agent");

		// A document written before activation existed has no `active` key on
		// its Agents; it has to load with every Agent activated. Only the
		// `active: false` lines come out - a path map carries its own `active`
		// field, which is older than activation and stays.
		auto legacy = yaml;
		for (;;)
		{
			auto const field = legacy.find("active: false");
			if (field == std::string::npos) break;
			auto const lineStart = legacy.rfind('\n', field);
			auto const lineEnd = legacy.find('\n', field);
			legacy.erase(lineStart, lineEnd - lineStart);
		}
		require(legacy.find("active: false") == std::string::npos,
			"The legacy fixture still mentions deactivation");
		auto const loadedLegacy = loadBuilding(legacy);
		require(findAgentByName(*loadedLegacy, "Parker")->isActive(),
			"A document without the activation field loaded a deactivated Agent");
		require(findAgentByName(*loadedLegacy, "Walker")->isActive(),
			"A document without the activation field lost an activation");
	}

	void activationSurvivesTopologyEdits()
	{
		WalkFixture fixture;
		std::string diagnostic;

		// Move the destination marker one cell to the left: an atomic replay
		// of the authored records, carrying every Agent back by hand.
		auto const plan = fixture.building.planMoveSectorObject(fixture.corridor, 0, 6, 0);
		require(plan.valid, "Moving the marker was refused: " + plan.diagnostic);
		fixture.building.pauseSimulation();
		require(fixture.building.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);
		(void)fixture.building.applyObjectMove(plan);

		require(!fixture.building.lookupAgent(fixture.second).entity->isActive(),
			"An object-move replay reactivated a deactivated Agent");
		require(fixture.building.lookupAgent(fixture.first).entity->isActive(),
			"An object-move replay deactivated an activated Agent");
	}

	// The clipboard envelope around a hand-written `object` map body, the
	// same envelope makeAgentClipboardText writes.
	std::string clipboardTextWithObjectBody(std::string const& body)
	{
		return "prometheumClipboard:\n  version: 1\n  operation: copy\n  type: Agent\n"
			"  object:\n" + body;
	}

	void clipboardCarriesActivation()
	{
		WalkFixture fixture;
		std::string diagnostic;

		fixture.building.pauseSimulation();
		require(fixture.building.setAgentActive(fixture.second, false, &diagnostic),
			"The deactivation was refused: " + diagnostic);

		auto const payload = makeAgentClipboardPayload(fixture.building, fixture.second, "Parked copy");
		require(!payload.active, "A copied deactivated Agent's payload stayed activated");
		auto const text = makeAgentClipboardText(payload, false);
		require(text.find("active") != std::string::npos,
			"A deactivated Agent's clipboard text does not mention activation");

		auto const parsed = YAML::Load(text);
		AgentClipboardPayload read;
		require(readAgentClipboardObject(parsed["prometheumClipboard"]["object"], read, diagnostic),
			"The clipboard text did not parse: " + diagnostic);
		require(!read.active, "A parsed payload lost the deactivation");

		// No `active` key - the shape every payload written before activation
		// existed has - reads back activated.
		AgentClipboardPayload legacy{ "Legacy", 0, true, std::nullopt };
		auto const legacyText = makeAgentClipboardText(legacy, false);
		require(legacyText.find("active") == std::string::npos,
			"An activated Agent's clipboard text mentions activation");
		AgentClipboardPayload legacyRead;
		require(readAgentClipboardObject(
			YAML::Load(legacyText)["prometheumClipboard"]["object"], legacyRead, diagnostic),
			"The legacy clipboard text did not parse: " + diagnostic);
		require(legacyRead.active, "A payload without the activation key did not read as activated");

		// A present key of the wrong shape is refused, not coerced: a
		// silently-activated paste of a parked Agent would start simulating
		// someone the author had parked.
		auto const malformedText = clipboardTextWithObjectBody(
			"    name: Broken\n    flags: 0\n    active: [not, a, bool]\n");
		AgentClipboardPayload malformedRead;
		require(!readAgentClipboardObject(
			YAML::Load(malformedText)["prometheumClipboard"]["object"], malformedRead, diagnostic),
			"A non-boolean activation was read instead of refused");
		require(!diagnostic.empty(), "A refused clipboard payload gave no reason");
	}
}

void runAgentActivationSmokeChecks()
{
	everyAgentStartsActivated();
	activationIsRefusedWhileTheSimulationRuns();
	deactivatedAgentsAreNotSimulated();
	reactivationWhilePausedPutsTheAgentBackUnderTheSimulation();
	wakingSkipsDeactivatedAgents();
	activationSurvivesSerializationAndReset();
	activationSurvivesTopologyEdits();
	clipboardCarriesActivation();
}
