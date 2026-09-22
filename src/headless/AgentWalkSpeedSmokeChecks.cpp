// Sampled Walk speed modifiers, ticket #134. These checks stay CPU-side while
// exercising registry/Building persistence, per-Agent draws, movement and route
// timing, inheritance conflicts, and the real Selection-panel text.

#include "AgentTagAssignmentPanel.h"

#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "imgui/imgui.h"

#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/Building.h"
#include "core/Defines.h"
#include "core/Graph.h"
#include "core/SerializationWorkData.h"
#include "core/Staircase.h"
#include "core/StaircaseEdge.h"
#include "core/YamlSerializer.h"

void runAgentWalkSpeedSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
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

	std::string serializeBuilding(core::Building const& building)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		building.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::AgentTagRegistry> deserializeRegistry(std::string const& yaml)
	{
		auto registry = core::AgentTagRegistry::create();
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(registry->deserialize(*reader, work),
			"The Walk speed registry did not deserialize");
		return registry;
	}

	std::shared_ptr<core::Building> deserializeBuilding(std::string const& yaml)
	{
		auto building = std::make_shared<core::Building>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(building->deserialize(*reader, work),
			"The Walk speed Building did not deserialize");
		return building;
	}

	void rangesAreBoundedRevisionedAndPersisted()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("walkers");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(tag, &diagnostic), diagnostic);
		auto const* added = registry->getAgentTagWalkSpeedModifier(tag);
		require(added && added->range == core::DefaultAgentWalkSpeedModifierRange
			&& added->revision == 1 && registry->getNextPropertyRevision() == 2,
			"Walk speed did not default both endpoints to 1.0 at revision 1");

		auto const beforeInvalid = serializeRegistry(*registry);
		auto const revisionBeforeInvalid = registry->getNextPropertyRevision();
		for (auto const invalid : {
			core::AgentModifierRange{ 0.79f, 1.0f },
			core::AgentModifierRange{ 1.0f, 1.21f },
			core::AgentModifierRange{ 1.1f, 1.0f },
			core::AgentModifierRange{ std::numeric_limits<float>::infinity(), 1.0f },
			core::AgentModifierRange{ 1.0f, std::numeric_limits<float>::quiet_NaN() } })
		{
			require(!registry->setAgentTagWalkSpeedModifier(tag, invalid, &diagnostic)
				&& !diagnostic.empty() && serializeRegistry(*registry) == beforeInvalid
				&& registry->getNextPropertyRevision() == revisionBeforeInvalid,
				"An invalid Walk speed range changed the registry or revision allocator");
		}

		require(registry->setAgentTagWalkSpeedModifier(tag, { 0.8f, 1.2f }, &diagnostic),
			"A legal Walk speed range was refused: " + diagnostic);
		auto const expected = *registry->getAgentTagWalkSpeedModifier(tag);
		require(expected.revision == 2 && registry->getNextPropertyRevision() == 3,
			"A real Walk speed range edit did not allocate one revision");
		auto const yaml = serializeRegistry(*registry);
		require(yaml.find("type: walkSpeedModifier") != std::string::npos,
			"Walk speed property type was not persisted");
		auto reopened = deserializeRegistry(yaml);
		require(*reopened->getAgentTagWalkSpeedModifier(tag) == expected,
			"Walk speed endpoints or revision did not survive registry persistence");

		auto malformed = YAML::Load(yaml);
		malformed["tags"][0]["properties"][0]["min"] = 0.7f;
		bool refused{ false };
		try { (void)deserializeRegistry(YAML::Dump(malformed)); }
		catch (std::exception const& error)
		{
			refused = std::string(error.what()).find("between 0.8 and 1.2")
				!= std::string::npos;
		}
		require(refused, "A persisted out-of-bounds Walk speed range was accepted");
	}

	void independentAndFixedSamplesPersistAcrossLoadAndReset()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const varied = registry->addAgentTag("varied");
		auto const fixed = registry->addAgentTag("fixed");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(varied, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(varied, { 0.8f, 1.2f }, &diagnostic),
			diagnostic);
		require(registry->addAgentTagWalkSpeedModifier(fixed, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(fixed, { 0.9f, 0.9f }, &diagnostic),
			diagnostic);

		auto building = std::make_shared<core::Building>("Samples", 8, 2);
		building->attachAgentTagRegistry("samples.tags.yaml", registry);
		auto const corridor = building->addCorridor(0, 0, 7);
		building->finishBuild();
		building->pauseSimulation();

		std::set<float> variedValues;
		core::AgentId retained{};
		for (int index = 0; index < 32; ++index)
		{
			auto const id = building->createAgent(
				"Varied " + std::to_string(index), corridor, 0, 1.0f);
			require(building->assignAgentTag(id, varied, &diagnostic), diagnostic);
			auto const& sample = building->lookupAgent(id).entity->getWalkSpeedModifierSample();
			require(sample && sample->type == core::SampledAgentPropertyType::WalkSpeedModifier
				&& sample->sourceTag == varied
				&& sample->propertyRevision
					== registry->getAgentTagWalkSpeedModifier(varied)->revision
				&& sample->value >= 0.8f && sample->value <= 1.2f,
				"A varied Agent sample lost its type, provenance, revision, or bounds");
			variedValues.insert(sample->value);
			if (index == 0) retained = id;
		}
		require(variedValues.size() > 1,
			"Independent non-degenerate Agent samples all received one shared value");

		auto const fixedAgent = building->createAgent("Fixed", corridor, 0, 2.0f);
		require(building->assignAgentTag(fixedAgent, fixed, &diagnostic), diagnostic);
		auto const fixedSample = building->lookupAgent(fixedAgent).entity
			->getWalkSpeedModifierSample();
		require(fixedSample && std::abs(fixedSample->value - 0.9f) < 0.000001f,
			"A degenerate Walk speed range did not return its exact endpoint");

		auto const retainedSample = *building->lookupAgent(retained).entity
			->getWalkSpeedModifierSample();
		auto const buildingYaml = serializeBuilding(*building);
		require(buildingYaml.find("type: walkSpeedModifier") != std::string::npos
			&& buildingYaml.find("sourceTag:") != std::string::npos
			&& buildingYaml.find("propertyRevision:") != std::string::npos,
			"Building persistence omitted Walk speed sample provenance");
		auto reopened = deserializeBuilding(buildingYaml);
		reopened->resolveAgentTagRegistry(registry);
		require(reopened->lookupAgent(retained).entity->getWalkSpeedModifierSample()
			== std::optional<core::AgentPropertySample>{ retainedSample },
			"The exact Walk speed sample did not survive save/load");

		reopened->resetSimulation();
		require(reopened->lookupAgent(retained).entity->getWalkSpeedModifierSample()
			== std::optional<core::AgentPropertySample>{ retainedSample },
			"Simulation reset rerolled or discarded the Walk speed sample");
	}

	void inheritedConflictsAreRefusedAtomically()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const first = registry->addAgentTag("first");
		auto const second = registry->addAgentTag("second");
		auto const pending = registry->addAgentTag("pending");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(first, &diagnostic), diagnostic);
		require(registry->addAgentTagWalkSpeedModifier(second, &diagnostic), diagnostic);

		auto building = std::make_shared<core::Building>("Conflicts", 6, 2);
		building->attachAgentTagRegistry("conflicts.tags.yaml", registry);
		auto const corridor = building->addCorridor(0, 0, 5);
		building->finishBuild();
		auto const agent = building->createAgent("Tagged", corridor);
		building->pauseSimulation();
		require(building->assignAgentTag(agent, first, &diagnostic), diagnostic);
		auto const buildingBefore = serializeBuilding(*building);
		require(!building->assignAgentTag(agent, second, &diagnostic)
			&& diagnostic.find("Walk speed modifier") != std::string::npos
			&& diagnostic.find("#first") != std::string::npos
			&& diagnostic.find("#second") != std::string::npos
			&& serializeBuilding(*building) == buildingBefore,
			"A duplicate inherited Walk speed assignment was not refused atomically");

		require(building->assignAgentTag(agent, pending, &diagnostic), diagnostic);
		auto const registryBefore = serializeRegistry(*registry);
		auto const revisionBefore = registry->getNextPropertyRevision();
		require(!registry->addAgentTagWalkSpeedModifier(pending, &diagnostic)
			&& diagnostic.find("Walk speed modifier") != std::string::npos
			&& diagnostic.find("#first") != std::string::npos
			&& serializeRegistry(*registry) == registryBefore
			&& registry->getNextPropertyRevision() == revisionBefore,
			"A conflicting property addition changed definitions or revisions");

		// Adding the property to an otherwise non-conflicting assigned tag samples
		// every assigned Agent at the property's exact default endpoint.
		auto const clean = registry->addAgentTag("clean");
		auto const cleanAgent = building->createAgent("Clean", corridor);
		require(building->assignAgentTag(cleanAgent, clean, &diagnostic), diagnostic);
		require(registry->addAgentTagWalkSpeedModifier(clean, &diagnostic), diagnostic);
		auto const generated = building->lookupAgent(cleanAgent).entity
			->getWalkSpeedModifierSample();
		require(generated && generated->sourceTag == clean
			&& std::abs(generated->value - 1.0f) < 0.000001f,
			"Adding Walk speed to an assigned tag did not create its Agent sample");
	}

	void movementRouteTimeAndExplicitSpeedsUseTheRightObservations()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const slowTag = registry->addAgentTag("slow");
		auto const fastTag = registry->addAgentTag("fast");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(slowTag, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(slowTag, { 0.8f, 0.8f }, &diagnostic),
			diagnostic);
		require(registry->addAgentTagWalkSpeedModifier(fastTag, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(fastTag, { 1.2f, 1.2f }, &diagnostic),
			diagnostic);

		auto building = std::make_shared<core::Building>("Walking", 12, 2);
		building->attachAgentTagRegistry("walking.tags.yaml", registry);
		auto const corridor = building->addCorridor(0, 0, 11);
		uint32_t sourceIdentifier{ 0x57313334u };
		uint32_t targetIdentifier{ 0x57313335u };
		building->addSectorMarker(corridor, 0, 1.5f, &sourceIdentifier);
		building->addSectorMarker(corridor, 0, 10.5f, &targetIdentifier);
		building->finishBuild();
		auto const slowId = building->createAgent("Slow", corridor, 0, 0.5f);
		auto const fastId = building->createAgent("Fast", corridor, 0, 0.5f);
		building->pauseSimulation();
		require(building->assignAgentTag(slowId, slowTag, &diagnostic), diagnostic);
		require(building->assignAgentTag(fastId, fastTag, &diagnostic), diagnostic);
		auto* slow = building->lookupAgent(slowId).entity;
		auto* fast = building->lookupAgent(fastId).entity;
		require(std::abs(slow->getWalkSpeed()
			- static_cast<float>(CORE_AGENT_BASE_WALK_SPEED) * 0.8f) < 0.00001f
			&& std::abs(fast->getWalkSpeed()
				- static_cast<float>(CORE_AGENT_BASE_WALK_SPEED) * 1.2f) < 0.00001f,
			"Base walk speed did not use the sampled modifier");
		require(std::abs(slow->getClimbSpeed() - fast->getClimbSpeed()) < 0.000001f
			&& std::abs(slow->getClimbSpeed()
				- static_cast<float>(CORE_AGENT_BASE_CLIMB_SPEED)) < 0.000001f,
			"Walk speed modifiers changed climb speed");

		auto const target = building->getGraph()->getVertexByIdentifier(targetIdentifier);
		auto slowPath = building->getGraph()->calculatePath(slow, target);
		auto fastPath = building->getGraph()->calculatePath(fast, target);
		require(slowPath && fastPath && slowPath->nodes.size() == fastPath->nodes.size()
			&& slowPath->nodes.back().edgeWeight > fastPath->nodes.back().edgeWeight,
			"Equal routes did not report a lower route time for the faster Agent");

		auto const slowStart = slow->getGlobalPosition();
		auto const fastStart = fast->getGlobalPosition();
		slow->setPath(std::move(slowPath), true);
		fast->setPath(std::move(fastPath), true);
		slow->update(0.1f);
		fast->update(0.1f);
		auto const slowDistance = slow->getGlobalPosition().distanceTo(slowStart);
		auto const fastDistance = fast->getGlobalPosition().distanceTo(fastStart);
		require(fastDistance > slowDistance
			&& std::abs(slowDistance - slow->getWalkSpeed() * 0.1f) < 0.0001f
			&& std::abs(fastDistance - fast->getWalkSpeed() * 0.1f) < 0.0001f,
			"Observable walking distance did not use each Agent's sample");

		auto staircase = std::make_shared<core::Staircase>(0, 0, 2, 1, 0.37f);
		core::StaircaseEdge escalator(staircase);
		require(std::abs(escalator.getTraversalSpeed(slow) - 0.37f) < 0.000001f
			&& std::abs(escalator.getTraversalSpeed(fast) - 0.37f) < 0.000001f,
			"An explicit transport movement speed was modified per Agent");
	}

	void captureClipboardText(void* userData, char const* text)
	{
		if (auto* writes = static_cast<std::vector<std::string>*>(userData))
			writes->emplace_back(text ? text : "");
	}

	char const* readClipboardText(void*) { return nullptr; }

	void selectionReportsSampleAndSource()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("sprinter");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(tag, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(tag, { 1.125f, 1.125f }, &diagnostic),
			diagnostic);
		auto building = std::make_shared<core::Building>("Inspection", 5, 2);
		building->attachAgentTagRegistry("inspection.tags.yaml", registry);
		auto const corridor = building->addCorridor(0, 0, 4);
		building->finishBuild();
		auto const sampled = building->createAgent("Sampled", corridor);
		auto const plain = building->createAgent("Plain", corridor);
		building->pauseSimulation();
		require(building->assignAgentTag(sampled, tag, &diagnostic), diagnostic);

		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(800.0f, 600.0f);
		io.Fonts->AddFontDefault();
		io.Fonts->Build();
		std::vector<std::string> clipboardWrites;
		io.SetClipboardTextFn = &captureClipboardText;
		io.GetClipboardTextFn = &readClipboardText;
		io.ClipboardUserData = &clipboardWrites;
		ImGui::NewFrame();
		ImGui::Begin("Selection");
		ImGui::LogToClipboard();
		renderAgentEffectiveProperties(building, sampled);
		renderAgentEffectiveProperties(building, plain);
		ImGui::End();
		ImGui::Render();
		std::string visible;
		for (auto const& text : clipboardWrites) visible += text;
		require(visible.find("Walk speed modifier: 1.125x from #sprinter")
				!= std::string::npos
			&& visible.find("Walk speed modifier: 1.000x (base default)")
				!= std::string::npos,
			"Selection omitted the sampled Walk speed, source tag, or base default");
		ImGui::DestroyContext();
	}
}

void runAgentWalkSpeedSmokeChecks()
{
	rangesAreBoundedRevisionedAndPersisted();
	independentAndFixedSamplesPersistAcrossLoadAndReset();
	inheritedConflictsAreRefusedAtomically();
	movementRouteTimeAndExplicitSpeedsUseTheRightObservations();
	selectionReportsSampleAndSource();
}
