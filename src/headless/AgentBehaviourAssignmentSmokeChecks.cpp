// Typed authored Agent behaviour assignment checks for #149.

#include <memory>
#include <stdexcept>
#include <string>

#include "AgentBehaviourAssignmentPanel.h"
#include "DocumentEdit.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/Building.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui.h"

void runAgentBehaviourAssignmentSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serialize(core::Building const& building)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		building.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::Building> deserialize(std::string const& yaml,
		std::shared_ptr<core::AgentBehaviourRegistry> const& registry)
	{
		auto result = std::make_shared<core::Building>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(result->deserialize(*reader, work), "Building assignment snapshot did not load");
		if (result->hasAgentBehaviourRegistryReference())
			result->resolveAgentBehaviourRegistry(registry);
		result->pauseSimulation();
		return result;
	}

	struct Fixture
	{
		std::shared_ptr<core::Building> building
			= std::make_shared<core::Building>("Assignments", 12, 2);
		std::shared_ptr<core::AgentBehaviourRegistry> registry
			= core::AgentBehaviourRegistry::create();
		core::AgentBehaviourId behaviour;
		core::AgentId first;
		core::AgentId second;
		core::MarkerId marker;

		Fixture()
		{
			auto room = building->addRoom("Room", 0, 0, 0, 12, 1);
			auto markerObject = building->addSectorMarker(room, 0, 8.5f, "Destination");
			(void)markerObject;
			marker = building->getMarkerIds().front();
			first = building->createAgent("Ada", room, 0, 1.5f);
			second = building->createAgent("Ben", room, 0, 2.5f);
			building->pauseSimulation();
			building->attachAgentBehaviourRegistry("assignments.behaviours", registry);
			std::vector<core::AgentBehaviourSchemaField> schema{
				{ "enabled", core::AgentBehaviourSchemaType::Boolean, {}, true, std::nullopt },
				{ "count", core::AgentBehaviourSchemaType::Integer, {}, true, std::nullopt },
				{ "weight", core::AgentBehaviourSchemaType::Number, {}, true, std::nullopt },
				{ "label", core::AgentBehaviourSchemaType::String, {}, false, std::string("default") },
				{ "delay", core::AgentBehaviourSchemaType::Duration, {}, true, std::nullopt },
				{ "destination", core::AgentBehaviourSchemaType::Marker, {}, true, std::nullopt }
			};
			behaviour = registry->addAgentBehaviour("Schedule", "schedule.lua", schema);
		}

		core::AgentBehaviourConfiguration configuration() const
		{
			return {
				{ "enabled", true }, { "count", int64_t{ 3 } }, { "weight", 2.5 },
				{ "delay", core::AgentBehaviourDuration{ 12 } }, { "destination", marker }
			};
		}
	};

	void assignEditClearUndoRedoAndPersistence()
	{
		Fixture fixture;
		gBuildingDocumentHistory.clear();
		std::string diagnostic;
		auto const revision = fixture.registry->lookupAgentBehaviour(fixture.behaviour)->getRevision();
		require(commitAgentBehaviourAssignment(fixture.building, fixture.first,
			fixture.behaviour, revision, fixture.configuration(), diagnostic),
			"Valid assignment was refused: " + diagnostic);
		auto assigned = fixture.building->getAgentBehaviourAssignment(fixture.first);
		require(assigned && assigned->configuration.size() == 6
			&& std::get<std::string>(assigned->configuration.at("label")) == "default",
			"Optional schema default was not materialized");

		auto edited = assigned->configuration;
		edited["count"] = int64_t{ 9 };
		require(commitAgentBehaviourAssignment(fixture.building, fixture.first,
			fixture.behaviour, revision, edited, diagnostic)
			&& gBuildingDocumentHistory.undoCount() == 2,
			"Configuration edit was not one undoable Building edit");

		auto current = fixture.building;
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			try { current = deserialize(snapshot.yaml, fixture.registry); return true; }
			catch (...) { return false; }
		};
		require(gBuildingDocumentHistory.undo(
			gBuildingDocumentHistory.capture(serialize(*current)), restore), "Undo failed");
		require(std::get<int64_t>(current->getAgentBehaviourAssignment(fixture.first)
			->configuration.at("count")) == 3, "Undo did not restore configuration");
		require(gBuildingDocumentHistory.redo(
			gBuildingDocumentHistory.capture(serialize(*current)), restore), "Redo failed");
		require(std::get<int64_t>(current->getAgentBehaviourAssignment(fixture.first)
			->configuration.at("count")) == 9, "Redo did not restore configuration edit");

		auto yaml = serialize(*current);
		require(yaml.find("version: 13") != std::string::npos
			&& yaml.find("type: marker") != std::string::npos,
			"Version-13 typed assignment was not persisted");
		auto reopened = deserialize(yaml, fixture.registry);
		require(reopened->getAgentBehaviourAssignment(fixture.first) ==
			current->getAgentBehaviourAssignment(fixture.first),
			"Typed assignment did not round-trip");
		require(commitAgentBehaviourClear(current, fixture.first, diagnostic)
			&& !current->getAgentBehaviourAssignment(fixture.first),
			"Assignment clear failed");
	}

	void validationAndPausedGateAreAtomic()
	{
		Fixture fixture;
		std::string diagnostic;
		auto const revision = fixture.registry->lookupAgentBehaviour(fixture.behaviour)->getRevision();
		auto valid = fixture.configuration();
		auto before = serialize(*fixture.building);
		auto expectRefused = [&](core::AgentBehaviourId behaviour, uint64_t candidateRevision,
			core::AgentBehaviourConfiguration configuration, std::string const& field)
		{
			require(!fixture.building->setAgentBehaviourAssignment(fixture.first, behaviour,
				candidateRevision, configuration, &diagnostic)
				&& diagnostic.find(field) != std::string::npos
				&& serialize(*fixture.building) == before,
				"Malformed configuration was not refused atomically with a field diagnostic");
		};
		auto missing = valid; missing.erase("count");
		expectRefused(fixture.behaviour, revision, missing, "count");
		auto unknown = valid; unknown["ghost"] = true;
		expectRefused(fixture.behaviour, revision, unknown, "ghost");
		auto wrong = valid; wrong["count"] = 1.0;
		expectRefused(fixture.behaviour, revision, wrong, "count");
		auto badMarker = valid; badMarker["destination"] = core::MarkerId{ 999 };
		expectRefused(fixture.behaviour, revision, badMarker, "destination");
		expectRefused(core::AgentBehaviourId{ 999 }, revision, valid, "999");
		expectRefused(fixture.behaviour, revision + 1, valid, "revision");

		fixture.building->finishBuild();
		require(fixture.building->resumeSimulation(), "Fixture could not run");
		require(!fixture.building->setAgentBehaviourAssignment(fixture.first,
			fixture.behaviour, revision, valid, &diagnostic)
			&& diagnostic.find("Pause") != std::string::npos,
			"Running assignment was accepted");
		fixture.building->pauseSimulation();
	}

	void markerDeletionReportsEveryReferenceAndPanelIsBalanced()
	{
		Fixture fixture;
		std::string diagnostic;
		auto const revision = fixture.registry->lookupAgentBehaviour(fixture.behaviour)->getRevision();
		require(fixture.building->setAgentBehaviourAssignment(fixture.first,
			fixture.behaviour, revision, fixture.configuration(), &diagnostic)
			&& fixture.building->setAgentBehaviourAssignment(fixture.second,
				fixture.behaviour, revision, fixture.configuration(), &diagnostic),
			"Marker-reference fixture assignments failed");
		auto marker = fixture.building->lookupMarker(fixture.marker);
		require(marker && !fixture.building->removeSectorMarker(0, 0, &diagnostic)
			&& diagnostic.find("Ada") != std::string::npos
			&& diagnostic.find("Ben") != std::string::npos
			&& diagnostic.find("destination") != std::string::npos
			&& fixture.building->lookupMarker(fixture.marker),
			"Referenced Marker deletion was not refused with every Agent and field");

		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800, 600);
		unsigned char* pixels; int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		ImGui::NewFrame();
		ImGui::Begin("Assignment panel smoke");
		renderAgentBehaviourAssignmentCell(fixture.building, fixture.first);
		renderAgentBehaviourConfigurationPanel(fixture.building, fixture.first);
		ImGui::End();
		ImGui::Render();
		ImGui::DestroyContext();
	}
}

void runAgentBehaviourAssignmentSmokeChecks()
{
	assignEditClearUndoRedoAndPersistence();
	validationAndPausedGateAreAtomic();
	markerDeletionReportsEveryReferenceAndPanelIsBalanced();
}
