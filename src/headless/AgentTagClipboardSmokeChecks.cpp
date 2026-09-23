// Same-registry Agent tag copy, cut and paste checks for ticket #139.
// Tagged payloads carry stable registry identity, complete assignments and exact
// modifier sample provenance. Untagged payloads remain registry-independent.

#include "AgentClipboard.h"
#include "DocumentEdit.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/World.h"
#include "core/Sector.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

void runAgentTagClipboardSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct World
	{
		std::shared_ptr<core::World> world;
		uint32_t corridor{ 0 };
	};

	World makeWorld(std::string const& name,
		std::shared_ptr<core::AgentTagRegistry> const& registry = {})
	{
		World world;
		world.world = std::make_shared<core::World>(name, 10, 2);
		if (registry) world.world->attachAgentTagRegistry(
			name + ".tags.yaml", registry);
		world.corridor = world.world->addCorridor(0, 0, 10);
		world.world->finishBuild();
		world.world->pauseSimulation();
		return world;
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

	std::shared_ptr<core::AgentTagRegistry> deserializeRegistry(
		std::string const& yaml)
	{
		auto registry = core::AgentTagRegistry::create();
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(registry->deserialize(*reader, work),
			"The cloned Agent tag registry did not deserialize");
		return registry;
	}

	std::size_t agentCount(core::World const& world)
	{
		return world.getSimulationSnapshot().agents.size();
	}

	struct TagFixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry;
		core::AgentTagId colour;
		core::AgentTagId walkSpeed;
		core::AgentTagId height;
	};

	TagFixture makeTagFixture()
	{
		TagFixture fixture;
		fixture.registry = core::AgentTagRegistry::create();
		fixture.colour = fixture.registry->addAgentTag("uniform");
		fixture.walkSpeed = fixture.registry->addAgentTag("pace");
		fixture.height = fixture.registry->addAgentTag("stature");
		std::string diagnostic;
		require(fixture.registry->addAgentTagColour(fixture.colour, &diagnostic),
			diagnostic);
		require(fixture.registry->setAgentTagColour(fixture.colour,
			{ 12, 34, 56 }, &diagnostic), diagnostic);
		require(fixture.registry->addAgentTagWalkSpeedModifier(
			fixture.walkSpeed, &diagnostic), diagnostic);
		require(fixture.registry->setAgentTagWalkSpeedModifier(
			fixture.walkSpeed, { 0.81f, 1.19f }, &diagnostic), diagnostic);
		require(fixture.registry->addAgentTagHeightModifier(
			fixture.height, &diagnostic), diagnostic);
		require(fixture.registry->setAgentTagHeightModifier(
			fixture.height, { 0.71f, 0.99f }, &diagnostic), diagnostic);
		return fixture;
	}

	core::AgentId createTaggedAgent(World const& world, TagFixture const& fixture,
		std::string const& name)
	{
		auto const id = world.world->createAgent(name, world.corridor, 0, 2.0f);
		std::string diagnostic;
		for (auto const tag : { fixture.colour, fixture.walkSpeed, fixture.height })
			require(world.world->assignAgentTag(id, tag, &diagnostic), diagnostic);
		return id;
	}

	AgentClipboardPayload readPayload(std::string const& text)
	{
		auto const object = YAML::Load(text)["prometheumClipboard"]["object"];
		AgentClipboardPayload payload;
		std::string diagnostic;
		require(readAgentClipboardObject(object, payload, diagnostic),
			"Reading a tagged Agent clipboard payload failed: " + diagnostic);
		return payload;
	}

	core::AgentId place(World const& world, AgentClipboardPayload const& payload)
	{
		core::AgentId placed{};
		std::string diagnostic;
		require(commitAgentPlacement(world.world, payload,
			world.world->getSector(world.corridor), 0, 5.0f, placed, diagnostic),
			"Placing a tagged Agent failed: " + diagnostic);
		return placed;
	}

	void payloadCarriesRegistryAssignmentsAndExactSamples()
	{
		auto const fixture = makeTagFixture();
		auto const source = makeWorld("Payload source", fixture.registry);
		auto const agent = createTaggedAgent(source, fixture, "Alice");
		auto const entity = source.world->lookupAgent(agent).entity;
		auto const expectedWalk = entity->getWalkSpeedModifierSample();
		auto const expectedHeight = entity->getHeightModifierSample();

		auto const text = makeAgentClipboardText(
			makeAgentClipboardPayload(*source.world, agent, "Alice copy"), false);
		auto const payload = readPayload(text);

		require(payload.agentTagRegistryUuid
			&& *payload.agentTagRegistryUuid == fixture.registry->getUuid(),
			"The clipboard payload lost the source registry UUID");
		require(payload.agentTags == entity->getAgentTagIds(),
			"The clipboard payload lost an Agent tag assignment");
		require(payload.walkSpeedModifierSample == expectedWalk
			&& payload.heightModifierSample == expectedHeight,
			"The clipboard payload changed a modifier value or its provenance");
		require(text.find("agentTagRegistryUuid") != std::string::npos
			&& text.find("propertyRevision") != std::string::npos
			&& text.find("sourceTag") != std::string::npos,
			"The clipboard text omits Agent tag identity or sample provenance");
	}

	void sameRegistryPasteRestoresExactStateAsOneEdit()
	{
		auto const fixture = makeTagFixture();
		auto const registryYaml = serializeRegistry(*fixture.registry);
		auto const destinationRegistry = deserializeRegistry(registryYaml);
		require(destinationRegistry != fixture.registry
			&& destinationRegistry->getUuid() == fixture.registry->getUuid(),
			"The fixture did not create distinct registry documents with one UUID");
		auto const source = makeWorld("Same source", fixture.registry);
		auto const destination = makeWorld("Same destination", destinationRegistry);
		auto const original = createTaggedAgent(source, fixture, "Alice");
		auto const sourceAgent = source.world->lookupAgent(original).entity;
		auto const payload = readPayload(makeAgentClipboardText(
			makeAgentClipboardPayload(*source.world, original, "Alice copy"), false));
		auto const sourceRegistryBefore = serializeRegistry(*fixture.registry);
		auto const destinationRegistryBefore = serializeRegistry(*destinationRegistry);
		gWorldDocumentHistory.clear();

		PendingAgentPlacement pending;
		std::string diagnostic;
		require(armAgentPlacement(pending, *destination.world, payload,
			destination.world->getSector(destination.corridor), 0, 5.0f, diagnostic),
			"A same-registry tagged paste was not armed: " + diagnostic);
		require(agentCount(*destination.world) == 0,
			"Arming a same-registry paste created an Agent early");
		core::AgentId pasted{};
		require(commitPendingAgentPlacement(pending, destination.world,
			pasted, diagnostic), "A same-registry tagged paste failed: " + diagnostic);
		auto const pastedAgent = destination.world->lookupAgent(pasted).entity;

		require(pastedAgent->getAgentTagIds() == sourceAgent->getAgentTagIds(),
			"Same-registry paste did not restore exact Agent tag assignments");
		require(pastedAgent->getWalkSpeedModifierSample()
			== sourceAgent->getWalkSpeedModifierSample()
			&& pastedAgent->getHeightModifierSample()
				== sourceAgent->getHeightModifierSample(),
			"Same-registry paste resampled a modifier instead of restoring it exactly");
		require(gWorldDocumentHistory.undoCount() == 1,
			"A tagged Agent paste was not exactly one World edit");
		require(serializeRegistry(*fixture.registry) == sourceRegistryBefore
			&& serializeRegistry(*destinationRegistry) == destinationRegistryBefore,
			"Same-UUID paste mutated its source or destination Agent tag registry");
	}

	void differentAndAbsentRegistryPasteAreRefusedAtomically()
	{
		auto const sourceFixture = makeTagFixture();
		auto const source = makeWorld("Refusal source", sourceFixture.registry);
		auto const original = createTaggedAgent(source, sourceFixture, "Alice");
		auto const payload = readPayload(makeAgentClipboardText(
			makeAgentClipboardPayload(*source.world, original, "Alice copy"), false));
		auto const otherRegistry = core::AgentTagRegistry::create();
		(void)otherRegistry->addAgentTag("local");
		auto const different = makeWorld("Different destination", otherRegistry);
		auto const absent = makeWorld("Absent destination");
		auto const sourceRegistryBefore = serializeRegistry(*sourceFixture.registry);
		auto const otherRegistryBefore = serializeRegistry(*otherRegistry);

		for (auto const& destination : { different, absent })
		{
			gWorldDocumentHistory.clear();
			PendingAgentPlacement pending;
			std::string diagnostic;
			require(!armAgentPlacement(pending, *destination.world, payload,
				destination.world->getSector(destination.corridor), 0, 4.0f,
				diagnostic), "A tagged paste into a different or absent registry armed");
			require(!pending.armed() && !diagnostic.empty(),
				"A refused tagged paste remained pending or gave no reason");

			core::AgentId placed{};
			diagnostic.clear();
			require(!commitAgentPlacement(destination.world, payload,
				destination.world->getSector(destination.corridor), 0, 4.0f,
				placed, diagnostic),
				"A tagged paste into a different or absent registry landed");
			require(!placed && agentCount(*destination.world) == 0,
				"A refused tagged paste created an Agent");
			require(!gWorldDocumentHistory.canUndo(),
				"A refused tagged paste committed a World edit");
		}

		require(serializeRegistry(*sourceFixture.registry) == sourceRegistryBefore
			&& serializeRegistry(*otherRegistry) == otherRegistryBefore,
			"A refused tagged paste mutated its source or destination registry");
	}

	void untaggedAgentsRemainPortableAcrossRegistryBoundaries()
	{
		auto const sourceRegistry = core::AgentTagRegistry::create();
		auto const source = makeWorld("Portable source", sourceRegistry);
		auto const agent = source.world->createAgent(
			"Visitor", source.corridor, 0, 2.0f);
		auto const text = makeAgentClipboardText(
			makeAgentClipboardPayload(*source.world, agent, "Visitor copy"), false);
		auto const payload = readPayload(text);
		require(payload.agentTags.empty() && !payload.agentTagRegistryUuid
			&& !payload.walkSpeedModifierSample && !payload.heightModifierSample,
			"An untagged payload was coupled to its source registry");

		auto const otherRegistry = core::AgentTagRegistry::create();
		for (auto const& destination : {
			makeWorld("Portable absent"),
			makeWorld("Portable different", otherRegistry),
			makeWorld("Portable same", sourceRegistry) })
		{
			gWorldDocumentHistory.clear();
			auto const pasted = place(destination, payload);
			auto const entity = destination.world->lookupAgent(pasted).entity;
			require(entity->getAgentTagIds().empty()
				&& !entity->getWalkSpeedModifierSample()
				&& !entity->getHeightModifierSample(),
				"An untagged paste invented Agent tag state");
		}
	}

	void cuttingATaggedAgentLeavesSharedTagsAndOtherAssignments()
	{
		auto const fixture = makeTagFixture();
		auto const world = makeWorld("Cut source", fixture.registry);
		auto const alice = createTaggedAgent(world, fixture, "Alice");
		auto const bob = createTaggedAgent(world, fixture, "Bob");
		auto const bobTags = world.world->lookupAgent(bob).entity->getAgentTagIds();
		auto const bobWalk = world.world->lookupAgent(bob).entity
			->getWalkSpeedModifierSample();
		auto const bobHeight = world.world->lookupAgent(bob).entity
			->getHeightModifierSample();
		auto const registryBefore = serializeRegistry(*fixture.registry);
		auto const payload = makeAgentClipboardPayload(*world.world, alice, "Alice");

		std::string diagnostic;
		require(cutAgent(world.world, alice, diagnostic),
			"Cutting a tagged Agent failed: " + diagnostic);
		require(!world.world->lookupAgent(alice)
			&& world.world->lookupAgent(bob).entity->getAgentTagIds() == bobTags,
			"Cut removed more than the selected Agent or disturbed another assignment");
		require(world.world->lookupAgent(bob).entity->getWalkSpeedModifierSample()
			== bobWalk
			&& world.world->lookupAgent(bob).entity->getHeightModifierSample()
				== bobHeight,
			"Cut changed another Agent's modifier samples");
		require(fixture.registry->lookupAgentTag(fixture.colour)
			&& fixture.registry->lookupAgentTag(fixture.walkSpeed)
			&& fixture.registry->lookupAgentTag(fixture.height)
			&& serializeRegistry(*fixture.registry) == registryBefore,
			"Cut deleted or changed shared Agent tags");
		require(payload.agentTags == bobTags,
			"The cut payload did not retain the removed Agent's assignments");
	}

	void incompleteTaggedPayloadsAreNotSilentlyDowngraded()
	{
		auto malformed = YAML::Load(
			"name: Alice\n"
			"flags: 0\n"
			"tags: [1]\n");
		AgentClipboardPayload payload;
		std::string diagnostic;
		require(!readAgentClipboardObject(malformed, payload, diagnostic)
			&& !diagnostic.empty(),
			"A tagged payload without registry identity was read as untagged");
	}
}

void runAgentTagClipboardSmokeChecks()
{
	payloadCarriesRegistryAssignmentsAndExactSamples();
	sameRegistryPasteRestoresExactStateAsOneEdit();
	differentAndAbsentRegistryPasteAreRefusedAtomically();
	untaggedAgentsRemainPortableAcrossRegistryBoundaries();
	cuttingATaggedAgentLeavesSharedTagsAndOtherAssignments();
	incompleteTaggedPayloadsAreNotSilentlyDowngraded();
}
