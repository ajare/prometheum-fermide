#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "RecentFiles.h"
#include "core/Building.h"
#include "core/Defines.h"
#include "core/Serializable.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	class SerializableProbe final : public core::Serializable
	{
		int32_t mValue{ 0 };
		bool mDeserializationResult{ true };

		bool childrenModified() const override
		{
			return false;
		}

		void serializeImpl(core::Serializer& serializer, core::SerializationWorkData&) const override
		{
			serializer.beginMap("");
			serializer.writeInt32("value", mValue);
			serializer.endMap();
		}

		bool deserializeImpl(core::Serializer& serializer, core::SerializationWorkData&) override
		{
			mValue = serializer.readInt32("value");
			return mDeserializationResult;
		}

	public:
		explicit SerializableProbe(bool deserializationResult = true)
			: mDeserializationResult(deserializationResult)
		{
		}

		int32_t value() const
		{
			return mValue;
		}
	};

	void stringYamlRoundTripsPrimitiveValues()
	{
		auto writer = core::YamlSerializer::toString();
		writer->beginMap("");
		writer->writeBool("enabled", true);
		writer->writeUint32("count", 42);
		writer->writeDouble("ratio", 1.25);
		writer->writeString("label", std::string("smoke"));
		writer->beginArray("items");
		writer->writeInt32("", -4);
		writer->writeInt32("", 9);
		writer->endArray();
		writer->endMap();
		writer->serialize();

		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(reader->hasField("count"), "YAML field lookup failed");
		require(reader->readBool("enabled"), "YAML bool did not round-trip");
		require(reader->readUint32("count") == 42, "YAML integer did not round-trip");
		require(std::abs(reader->readDouble("ratio") - 1.25) < 0.0001,
			"YAML double did not round-trip");
		require(reader->readString("label") == "smoke", "YAML string did not round-trip");
		require(reader->readBool("missing", true, true), "optional YAML bool ignored its default");

		reader->beginArray("items");
		require(reader->nextArrayItem() && reader->readInt32() == -4,
			"first YAML array item did not round-trip");
		require(reader->nextArrayItem() && reader->readInt32() == 9,
			"second YAML array item did not round-trip");
		require(!reader->nextArrayItem(), "YAML array reported a nonexistent item");
		reader->endArray();
	}

	void fileYamlRoundTrips()
	{
		std::string const path = "serialization-smoke.yaml";
		struct FileCleanup
		{
			std::string const& path;
			~FileCleanup() { std::remove(path.c_str()); }
		} cleanup{ path };

		auto writer = core::YamlSerializer::toFile(path);
		writer->beginMap("");
		writer->writeString("source", std::string("file"));
		writer->endMap();
		writer->serialize();

		auto reader = core::YamlSerializer::fromFile(path);
		reader->deserialize();
		require(reader->readString("source") == "file", "YAML file did not round-trip");
	}

	void malformedValuesAndInvalidUsageThrowUsefulErrors()
	{
		auto reader = core::YamlSerializer::fromString("count: nope\n");
		reader->deserialize();
		try
		{
			(void)reader->readUint32("count");
			throw std::runtime_error("malformed required YAML value was accepted");
		}
		catch (core::SerializationException const& exception)
		{
			auto const message = std::string(exception.what());
			require(message.find("uint32") != std::string::npos
				&& message.find("/count") != std::string::npos,
				"malformed YAML error did not identify its type and path");
		}
		require(reader->readUint32("missing", true, 17) == 17,
			"optional malformed YAML value ignored its default");

		auto writer = core::YamlSerializer::toString();
		try
		{
			writer->writeInt32("outside", 1);
			throw std::runtime_error("YAML write outside a container was accepted");
		}
		catch (core::SerializationException const&)
		{
		}
		try
		{
			(void)writer->nextArrayItem();
			throw std::runtime_error("array iteration while serializing was accepted");
		}
		catch (core::SerializationException const&)
		{
		}
	}

	void buildingRoundTripsAuthoredStateAndAgents()
	{
		core::Building original("Serializable building", 8, 3);
		auto const fore = original.addRoom("Fore room", CORE_LAYER_FORE, 0, 0, 7, 2);
		original.addRoom("Back room", CORE_LAYER_BACK, 0, 0, 7, 2);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.activationMode = core::DoorActivationMode::RemoteControlled;
		doorOptions.controls[0] = true;
		doorOptions.controls[1] = true;
		doorOptions.crossingLanes = 2;
		original.addSectorDoor(0, 3, doorOptions);
		auto const removedMarker = original.addSectorMarker(fore, 0, 1.5f);
		original.addSectorMarker(fore, 0, 2.5f);
		require(original.removeSectorMarker(fore, removedMarker.index),
			"Marker could not be removed through Building");
		require(!original.removeSectorMarker(fore, removedMarker.index),
			"Marker deletion accepted an empty object slot");
		original.finishBuild();
		auto const agentId = original.createAgent("Serialized agent", fore, 0, 0.75f);
		original.lookupAgent(agentId).entity->setFlags(0x12u);

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("version: 2") != std::string::npos
			&& yaml.find("type: room") != std::string::npos
			&& yaml.find("cellsWide:") != std::string::npos
			&& yaml.find("foreControl: true") != std::string::npos
			&& yaml.find("\n    a:") == std::string::npos,
			"Building YAML did not use the explicit construction schema");
		require(yaml.find("construction") != std::string::npos
			&& yaml.find("agents") != std::string::npos,
			"Building YAML omitted authored structure or agents");

		core::Building loaded("placeholder", 2, 2);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Building deserialization failed");
		require(loaded.getName() == original.getName()
			&& loaded.getCellsWide() == original.getCellsWide()
			&& loaded.getDecksHigh() == original.getDecksHigh()
			&& loaded.getNumSectors() == original.getNumSectors(),
			"Building metadata or sectors did not round-trip");
		require(loaded.getGraph() && !loaded.getGraph()->getVertices().empty(),
			"Building graph was not regenerated after deserialization");
		uint32_t markerCount{ 0 };
		auto const loadedFore = loaded.getSector(fore);
		for (uint32_t i = 0; i < loadedFore->getNumObjects(); ++i)
		{
			auto const object = loadedFore->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::Marker) ++markerCount;
		}
		require(markerCount == 1, "Marker deletion did not round-trip");
		auto const loadedAgent = loaded.lookupAgent(agentId);
		require(loadedAgent && loadedAgent.entity->getName() == "Serialized agent"
			&& loadedAgent.entity->getFlags() == 0x12u
			&& loadedAgent.entity->getSector()->getIndex() == fore
			&& std::abs(loadedAgent.entity->getLocalPosition().x - 0.75f) < 0.0001f
			&& loadedAgent.entity->getState() == core::Agent::State::Idle
			&& !loadedAgent.entity->getPath(),
			"Building-owned Agent did not round-trip as an idle, pathless Agent");
		require(!loaded.isModified(), "deserialized Building was unexpectedly modified");
		require(loaded.removeAgent(agentId).removed, "deserialized Agent could not be removed");
		require(loaded.isModified(), "removing an Agent did not modify its Building");
	}

	void legacyBuildingYamlStillLoads()
	{
		auto const yaml = R"yaml(version: 1
name: Legacy
cellsWide: 4
decksHigh: 2
construction:
  - kind: 0
    name: ""
    a: 0
    b: 0
    c: 4
    d: 1
    e: 0
    f: 0
    g: 0
    i: 0
    j: 0
    x: 0
    y: 0
    p: 0
    q: 0
    values: []
agents: []
)yaml";
		core::Building loaded("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "version 1 Building YAML no longer loads");
		require(loaded.getName() == "Legacy" && loaded.getNumSectors() == 1,
			"version 1 Building YAML loaded incorrectly");
	}

	void locationEditsArePlannedAndAppliedAtomically()
	{
		core::Building building("Editable", 8, 3);
		auto room = building.addRoom("Room", CORE_LAYER_FORE, 0, 0, 5, 2);
		building.addSectorMarker(room, 0, 4.5f);
		auto removed = building.addSectorMarker(room, 0, 1.5f);
		building.removeSectorMarker(room, removed.index);
		building.addSectorMarker(room, 0, 2.5f);
		building.finishBuild();
		auto agent = building.createAgent("Cropped", room, 0, 4.5f);

		auto resize = building.planResizeLocation(room, 0, 0, 3, 2);
		require(resize.valid && resize.requiresConfirmation(),
			"Location shrink did not report its cascading deletions");
		building.pauseSimulation();
		auto resized = building.applyLocationEdit(resize);
		require(building.getSector(resized)->getCellsWide() == 3,
			"Location width was not changed");
		require(!building.lookupAgent(agent), "Agent cropped by resize was retained");
		uint32_t retainedObjects = 0;
		for (uint32_t i = 0; i < building.getSector(resized)->getNumObjects(); ++i)
			if (building.getSector(resized)->getObject(i)) ++retainedObjects;
		require(retainedObjects == 1, "Resize did not preserve the correct authored object slots");
		require(building.isSimulationPaused(), "Location edit resumed the simulation");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		core::Building reloaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(reloaded.deserialize(*reader, workData)
			&& reloaded.getSector(resized)->getCellsWide() == 3,
			"Edited Location did not survive serialization");
		uint32_t reloadedObjects = 0;
		for (uint32_t i = 0; i < reloaded.getSector(resized)->getNumObjects(); ++i)
			if (reloaded.getSector(resized)->getObject(i)) ++reloadedObjects;
		require(reloadedObjects == 1, "Edited object tombstones did not survive serialization");

		auto movedAgent = building.createAgent("Moved", resized, 0, 1.0f);
		auto move = building.planResizeLocation(resized, 4, 0, 3, 2);
		if (!move.valid || !move.move || move.requiresConfirmation())
			throw std::runtime_error("Free Location move was not planned without deletions: "
				+ move.diagnostic + " consequences=" + std::to_string(move.consequences.size()));
		auto moved = building.applyLocationEdit(move);
		require(building.getSector(moved)->getCellX() == 4
			&& building.getSector(moved)->getCellY() == 0,
			"Location was not moved to its planned cells");
		auto movedLookup = building.lookupAgent(movedAgent);
		require(movedLookup && std::abs(movedLookup.entity->getGlobalPosition().x - 5.0f) < 0.001f
			&& std::abs(movedLookup.entity->getGlobalPosition().y) < 0.001f,
			"Agent did not move with its Location");
		std::shared_ptr<const core::SectorObject> movedObject;
		for (uint32_t i = 0; i < building.getSector(moved)->getNumObjects(); ++i)
			if (building.getSector(moved)->getObject(i)) movedObject = building.getSector(moved)->getObject(i);
		require(movedObject && movedObject->getCellX() == 6,
			"Sector object did not move with its Location");

		auto remove = building.planRemoveLocation(moved);
		require(remove.valid, "Valid Location deletion was rejected");
		building.applyLocationEdit(remove);
		require(building.getNumSectors() == 0, "Deleted Location was retained");

		core::Building fullWidthBuilding("Full width", 16, 2);
		auto createdFullWidth = fullWidthBuilding.addCorridor(0, 0, 16);
		require(fullWidthBuilding.getSector(createdFullWidth)->getCellX1() == 15,
			"Location creation did not include the final world column");

		core::Building boundaryBuilding("Boundary", 16, 2);
		auto boundaryCorridor = boundaryBuilding.addCorridor(0, 0, 15);
		boundaryBuilding.finishBuild();
		auto boundaryResize = boundaryBuilding.planResizeLocation(boundaryCorridor, 0, 0, 16, 1);
		require(boundaryResize.valid,
			"Location could not be resized through the final world column");
		boundaryBuilding.pauseSimulation();
		auto fullWidthCorridor = boundaryBuilding.applyLocationEdit(boundaryResize);
		require(boundaryBuilding.getSector(fullWidthCorridor)->getCellX1() == 15,
			"Location resize did not include the final world column");
	}

	float controlCenterX(core::Building::CreateObjectResult const& control)
	{
		auto object = control.sector->getObject(control.index)->_getObject();
		return object->getPosition().x + object->getSize().x * 0.5f;
	}

	float controlCenterY(core::Building::CreateObjectResult const& control)
	{
		auto object = control.sector->getObject(control.index)->_getObject();
		return object->getPosition().y + object->getSize().y * 0.5f;
	}

	void physicalControlsPreferDistinctWallPositions()
	{
		core::Building building("Control placement", 9, 2);
		building.addCorridor(0, 1, 7);
		auto room = building.addRoom("Back room", CORE_LAYER_BACK, 0, 4, 3, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[CORE_LAYER_BACK] = true;

		auto first = building.addSectorDoor(0, 5, options);
		auto second = building.addSectorDoor(0, 6, options);
		require(std::abs(controlCenterX(first.controls[CORE_LAYER_BACK]) - 5.0f) < 0.0001f
			&& std::abs(controlCenterX(second.controls[CORE_LAYER_BACK]) - 6.0f) < 0.0001f,
			"Adjacent Citadel-style Door controls did not choose distinct X positions");
		auto standardY = CORE_BUTTON_Y_OFFSET
			+ first.controls[CORE_LAYER_BACK].sector->getObject(first.controls[CORE_LAYER_BACK].index)
				->_getObject()->getSize().y * 0.5f;
		require(std::abs(controlCenterY(first.controls[CORE_LAYER_BACK]) - standardY) < 0.0001f
			&& std::abs(controlCenterY(second.controls[CORE_LAYER_BACK]) - standardY) < 0.0001f,
			"Separated Door controls retained obsolete height offsets");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		core::Building replayed("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(replayed.deserialize(*reader, workData), "Control-placement replay failed");
		std::vector<float> replayedCenters;
		for (uint32_t i = 0; i < replayed.getSector(room)->getNumObjects(); ++i)
		{
			auto object = replayed.getSector(room)->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::InteractionPoint)
				replayedCenters.push_back(object->_getObject()->getPosition().x
					+ object->_getObject()->getSize().x * 0.5f);
		}
		std::sort(replayedCenters.begin(), replayedCenters.end());
		require(replayedCenters.size() == 2
			&& std::abs(replayedCenters[0] - 5.0f) < 0.0001f
			&& std::abs(replayedCenters[1] - 6.0f) < 0.0001f,
			"Control placement was not deterministic after YAML replay");

		building.finishBuild();
		building.pauseSimulation();
		require(building.removeSectorDoor(second.door.sector->getIndex(), second.door.index),
			"Adjacent Door could not be removed");
		std::vector<float> remainingCenters;
		for (uint32_t i = 0; i < building.getSector(room)->getNumObjects(); ++i)
		{
			auto object = building.getSector(room)->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::InteractionPoint)
				remainingCenters.push_back(object->_getObject()->getPosition().x
					+ object->_getObject()->getSize().x * 0.5f);
		}
		require(remainingCenters.size() == 1
			&& std::abs(remainingCenters[0] - 6.0f) < 0.0001f,
			"Remaining Door control did not return to its preferred position after removal");

		core::Building fallback("Control fallback", 4, 2);
		fallback.addCorridor(0, 0, 2);
		fallback.addRoom("Narrow back room", CORE_LAYER_BACK, 0, 0, 2, 1);
		auto left = fallback.addSectorDoor(0, 0, options);
		auto right = fallback.addSectorDoor(0, 1, options);
		require(std::abs(controlCenterX(left.controls[CORE_LAYER_BACK])
				- controlCenterX(right.controls[CORE_LAYER_BACK])) < 0.0001f
			&& std::abs(controlCenterY(left.controls[CORE_LAYER_BACK])
				- controlCenterY(right.controls[CORE_LAYER_BACK])) > 0.049f,
			"Unavoidable same-X controls did not use the height fallback");
	}

	void recentFilesPersistAcrossStartup()
	{
		auto directory = std::filesystem::temp_directory_path() / "prometheum-fermide-recent-files-smoke";
		std::filesystem::remove_all(directory);
		std::filesystem::create_directories(directory);
		auto file = directory / "recent-files.txt";
		RecentFiles first(3);
		first.initialize(file);
		require(std::filesystem::exists(file), "Recent-file storage was not created on first startup");
		first.add("/tmp/alpha.yaml");
		first.add("/tmp/beta.yaml");
		first.add("/tmp/alpha.yaml");
		RecentFiles restarted(3);
		restarted.initialize(file);
		require(restarted.entries().size() == 2,
			"Recent files were not restored after startup");
		require(restarted.entries()[0] == "/tmp/alpha.yaml"
			&& restarted.entries()[1] == "/tmp/beta.yaml",
			"Recent files did not retain most-recent-first order or deduplication");
		std::filesystem::remove_all(directory);
	}

	void serializableTracksModificationState()
	{
		SerializableProbe probe;
		require(probe.isModified(), "new Serializable was unexpectedly unmodified");

		core::SerializationWorkData retainedState;
		retainedState.markSerializedUnmodified = false;
		auto retainedWriter = core::YamlSerializer::toString();
		probe.serialize(*retainedWriter, retainedState);
		retainedWriter->serialize();
		require(probe.isModified(), "serialization cleared modification state when disabled");

		core::SerializationWorkData defaultState;
		auto writer = core::YamlSerializer::toString();
		probe.serialize(*writer, defaultState);
		writer->serialize();
		require(!probe.isModified(), "serialization did not clear modification state");

		auto reader = core::YamlSerializer::fromString("value: 23\n");
		reader->deserialize();
		require(probe.deserialize(*reader, defaultState) && probe.value() == 23,
			"Serializable did not deserialize its value");
		require(!probe.isModified(), "deserialization left Serializable modified");

		SerializableProbe failed(false);
		auto failedReader = core::YamlSerializer::fromString("value: 7\n");
		failedReader->deserialize();
		require(!failed.deserialize(*failedReader, defaultState),
			"Serializable did not return its implementation's failure");
		require(!failed.isModified(), "failed deserialization left Serializable modified");
	}
}

void runSerializationSmokeChecks()
{
	stringYamlRoundTripsPrimitiveValues();
	fileYamlRoundTrips();
	malformedValuesAndInvalidUsageThrowUsefulErrors();
	buildingRoundTripsAuthoredStateAndAgents();
	legacyBuildingYamlStillLoads();
	locationEditsArePlannedAndAppliedAtomically();
	physicalControlsPreferDistinctWallPositions();
	recentFilesPersistAcrossStartup();
	serializableTracksModificationState();
}
