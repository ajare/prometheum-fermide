#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "RecentFiles.h"
#include "Render.h"
#include "UI.h"
#include "core/Building.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Defines.h"
#include "core/Serializable.h"
#include "core/SerializationException.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/Stairwell.h"
#include "core/StairwellTransit.h"
#include "core/Staircase.h"
#include "core/StaircaseTransit.h"
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
		auto const fore = original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.activationMode = core::DoorActivationMode::RemoteControlled;
		doorOptions.controls[0] = true;
		doorOptions.controls[1] = true;
		doorOptions.crossingLanes = 2;
		original.addSectorDoor(0, 3, doorOptions);
		auto const removedMarker = original.addSectorMarker(fore, 0, 1.5f);
		uint32_t destinationIdentifier{ 0x53455231u };
		original.addSectorMarker(fore, 0, 2.5f, &destinationIdentifier);
		require(original.removeSectorMarker(fore, removedMarker.index),
			"Marker could not be removed through Building");
		require(!original.removeSectorMarker(fore, removedMarker.index),
			"Marker deletion accepted an empty object slot");
		original.finishBuild();
		auto const agentId = original.createAgent("Serialized agent", fore, 0, 0.75f);
		auto* originalAgent = original.lookupAgent(agentId).entity;
		originalAgent->setFlags(0x12u);
		auto destination = original.getGraph()->getVertexByIdentifier(destinationIdentifier);
		auto path = original.getGraph()->calculatePath(originalAgent, destination);
		require(path && !path->nodes.empty(), "Agent path could not be created for serialization");
		originalAgent->setPath(std::move(path), true);

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("version: 4") != std::string::npos
			&& yaml.find("layers: 2") != std::string::npos
			&& yaml.find("layerNames:") != std::string::npos
			&& yaml.find("- Layer 0") != std::string::npos
			&& yaml.find("- Layer 1") != std::string::npos
			&& yaml.find("type: room") != std::string::npos
			&& yaml.find("cellsWide:") != std::string::npos
			&& yaml.find("foreControl: true") != std::string::npos
			&& yaml.find("\n    a:") == std::string::npos,
			"Building YAML did not use the explicit construction schema");
		require(yaml.find("construction") != std::string::npos
			&& yaml.find("agents") != std::string::npos
			&& yaml.find("path:") != std::string::npos
			&& yaml.find("destinationSector:") != std::string::npos
			&& yaml.find("active: true") != std::string::npos,
			"Building YAML omitted authored structure, agents, or Agent paths");

		core::Building loaded("placeholder", 2, 2);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Building deserialization failed");
		require(loaded.getName() == original.getName()
			&& loaded.getCellsWide() == original.getCellsWide()
			&& loaded.getDecksHigh() == original.getDecksHigh()
			&& loaded.getLayerCount() == original.getLayerCount()
			&& loaded.getLayerName(0) == "Layer 0"
			&& loaded.getLayerName(1) == "Layer 1"
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
			&& loadedAgent.entity->getState() == core::Agent::State::MovingToVertex
			&& loadedAgent.entity->getPath()
			&& loadedAgent.entity->getPath()->nodes.back().targetVertex->getSector()->getIndex() == fore
			&& std::abs(loadedAgent.entity->getPath()->nodes.back().targetVertex->getSectorOffset().x
				- destination->getSectorOffset().x) < 0.0001f,
			"Building-owned Agent or its active path did not round-trip");
		require(!loaded.isModified(), "deserialized Building was unexpectedly modified");
		auto replacementPath = loaded.getGraph()->calculatePath(loadedAgent.entity,
			loadedAgent.entity->getPath()->nodes.back().targetVertex);
		require(replacementPath && !replacementPath->nodes.empty(),
			"Replacement Agent path could not be created");
		loadedAgent.entity->setPath(std::move(replacementPath), false);
		require(loaded.isModified(), "Setting an Agent path did not mark its Building modified");
		loadedAgent.entity->clearPath();
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

	void legacyVersion3BuildingYamlStillLoadsWithDefaultLayers()
	{
		auto const yaml = R"yaml(version: 3
name: Legacy v3
cellsWide: 4
decksHigh: 2
construction:
  - type: corridor
    y: 0
    x: 0
    cellsWide: 4
    decksHigh: 1
agents: []
)yaml";
		core::Building loaded("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "version 3 Building YAML no longer loads");
		require(loaded.getName() == "Legacy v3"
			&& loaded.getLayerCount() == 2
			&& loaded.getLayerName(0) == "Layer 0"
			&& loaded.getLayerName(1) == "Layer 1",
			"version 3 Building YAML loaded with wrong layer defaults");
	}

	void buildingLayerNamesRoundTrip()
	{
		core::Building original("Named layers", 4, 2);
		original.setLayerName(0, "Front");
		original.setLayerName(1, "Rear");
		original.addCorridor(0, 0, 4);
		original.finishBuild();

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("layerNames:") != std::string::npos
			&& yaml.find("- Front") != std::string::npos
			&& yaml.find("- Rear") != std::string::npos,
			"Custom layer names were not serialized");

		core::Building loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Named layer Building did not deserialize");
		require(loaded.getLayerCount() == 2
			&& loaded.getLayerName(0) == "Front"
			&& loaded.getLayerName(1) == "Rear",
			"Custom layer names did not round-trip");
	}

	void layerFieldsAcceptLegacyNamesAndIndices()
	{
		auto const yaml = R"yaml(version: 4
name: Mixed layer spellings
cellsWide: 6
decksHigh: 2
layers: 3
layerNames:
  - Ground
  - Mezzanine
  - Sublevel
construction:
  - type: room
    name: Front
    layer: fore
    y: 0
    x: 0
    cellsWide: 3
    decksHigh: 1
    topDeckHeight: 0.9
  - type: room
    name: Deep
    layer: 2
    y: 0
    x: 3
    cellsWide: 3
    decksHigh: 1
    topDeckHeight: 0.9
agents: []
)yaml";
		core::Building loaded("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData),
			"A Building mixing legacy layer names and layer indices did not load");
		core::Building const& loadedRef = loaded;
		require(loadedRef.getNumSectors() == 2
			&& loadedRef.getSector(0)->getLayerIndex() == 0
			&& loadedRef.getSector(1)->getLayerIndex() == 2,
			"Legacy and indexed layers were not resolved to the right layers");
	}

	void addedLayersAppendToTheBackAndRoundTrip()
	{
		core::Building original("Growing", 4, 2);
		original.addCorridor(0, 0, 4);
		require(original.getLayerCount() == 2, "A new Building does not start with two layers");

		auto const appended = original.addLayer();
		require(appended == 2 && original.getLayerCount() == 3,
			"addLayer() did not append a third layer");
		require(original.getLayerName(2) == "Layer 2",
			"addLayer() did not name the new layer by its position");
		require(original.getLayerName(0) == "Layer 0" && original.getLayerName(1) == "Layer 1",
			"addLayer() disturbed the names of existing layers");

		original.setLayerName(2, "Sub-basement");
		original.addRoom("Store", 2, 0, 0, 3, 1);
		original.finishBuild();

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("layers: 3") != std::string::npos
			&& yaml.find("- Sub-basement") != std::string::npos,
			"The added layer was not serialized");

		core::Building loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "The three-layer Building did not deserialize");
		require(loaded.getLayerCount() == 3
			&& loaded.getLayerName(0) == "Layer 0"
			&& loaded.getLayerName(1) == "Layer 1"
			&& loaded.getLayerName(2) == "Sub-basement",
			"The added layer did not round-trip");
		core::Building const& loadedRef = loaded;
		require(loadedRef.getLayer(2) && loadedRef.getLayer(2)->getZ() == 2,
			"The added layer was not created at the expected depth");
	}

	void layerCountIsCappedAtCoreMaxLayers()
	{
		core::Building building("Capped", 1, 1);
		while (building.getLayerCount() < CORE_MAX_LAYERS) building.addLayer();
		require(building.getLayerCount() == CORE_MAX_LAYERS,
			"addLayer() stopped short of CORE_MAX_LAYERS");

		bool rejected = false;
		try
		{
			building.addLayer();
		}
		catch (std::exception const&)
		{
			rejected = true;
		}
		require(rejected, "addLayer() did not reject going beyond CORE_MAX_LAYERS");
		require(building.getLayerCount() == CORE_MAX_LAYERS,
			"A rejected addLayer() still changed the layer count");
	}

	void deletingAMiddleLayerCompactsTheLayersAboveIt()
	{
		core::Building building("Compacting", 8, 3);
		building.addLayer();
		building.addCorridor(0, 0, 8);
		building.addRoom("Basement", 1, 0, 0, 8, 1);
		building.addRoom("Cellar", 2, 0, 0, 8, 1);
		building.setLayerName(2, "Deep Cellar");
		building.addSectorDoor(0, 2);
		building.addSectorWindow(2, 0, 5, 1, 1);
		building.finishBuild();
		auto const survivor = building.createAgent("Walker", 0, 0, 1.0f);
		auto const buried = building.createAgent("Buried", 1, 0, 1.0f);

		building.pauseSimulation();
		auto const plan = building.planDeleteLayer(1);
		require(plan.valid, ("Middle layer deletion was rejected: " + plan.diagnostic).c_str());
		require(plan.layerCountBefore == 3 && plan.layerCountAfter == 2,
			"Layer deletion did not report its compaction");
		require(plan.locationsRemoved == 1 && plan.doorsRemoved == 1
			&& plan.windowsRemoved == 1 && plan.agentsRemoved == 1,
			"Layer deletion did not report its destructive consequences");
		require(plan.requiresConfirmation(),
			"A destructive layer deletion did not require confirmation");
		require(!plan.consequences.empty(), "Layer deletion produced no consequence list");
		require(building.applyDeleteLayer(plan), "Layer deletion was not applied");

		require(building.getLayerCount() == 2, "Layers were not compacted");
		require(building.getLayerName(1) == "Deep Cellar",
			"Layer names did not travel with the compacted layer");
		require(building.getNumSectors() == 2, "Sectors were not removed with their layer");
		require(building.getSector(0)->getLayerIndex() == 0
			&& building.getSector(1)->getName() == "Cellar"
			&& building.getSector(1)->getLayerIndex() == 1,
			"Higher layers were not compacted down by one");
		require(building.lookupAgent(survivor).entity != nullptr,
			"Agent outside the deleted layer was removed");
		require(building.lookupAgent(buried).entity == nullptr,
			"Agent in the deleted layer was retained");
		require(building.isSimulationPaused(), "Layer deletion resumed the simulation");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		core::Building reloaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(reloaded.deserialize(*reader, workData),
			"A compacted Building did not round-trip");
		require(reloaded.getLayerCount() == 2
			&& reloaded.getLayerName(1) == "Deep Cellar"
			&& reloaded.getNumSectors() == 2
			&& reloaded.getSector(1)->getName() == "Cellar",
			"Layer compaction did not survive serialization");
	}

	void deletingTheFrontLayerRemovesTransitsOneLayerBehind()
	{
		core::Building building("Front deletion", 8, 3);
		building.addLayer();
		building.addCorridor(0, 0, 8);
		building.addCorridor(2, 0, 8);
		building.addRoom("Deep", 2, 0, 0, 8, 3);
		building.addLadder(0, 3, { 3, false, true });
		building.finishBuild();
		auto const climber = building.createAgent("Climber", 3, 0, 0.5f);

		building.pauseSimulation();
		auto const plan = building.planDeleteLayer(0);
		require(plan.valid, ("Front layer deletion was rejected: " + plan.diagnostic).c_str());
		require(plan.locationsRemoved == 2, "Front layer Sectors were not counted");
		require(plan.transitsRemoved == 1,
			"Transits one layer behind the deletion were not counted");
		require(plan.agentsRemoved == 1, "Agents in the deleted Transit were not counted");
		require(building.applyDeleteLayer(plan), "Front layer deletion was not applied");
		require(building.getLayerCount() == 2, "Layers were not compacted");
		require(building.getNumSectors() == 1
			&& building.getSector(0)->getName() == "Deep"
			&& building.getSector(0)->getLayerIndex() == 1,
			"The surviving Room did not compact to the layer behind the deletion");
		require(building.lookupAgent(climber).entity == nullptr,
			"Agent in a removed Transit was retained");
	}

	void layerDeletionKeepsAtLeastTwoLayers()
	{
		core::Building building("Two layers", 4, 2);
		building.addCorridor(0, 0, 4);
		building.finishBuild();

		auto const last = building.planDeleteLayer(1);
		require(!last.valid && !last.diagnostic.empty(),
			"Deleting down to a single layer was not rejected");

		bool threw = false;
		try
		{
			building.applyDeleteLayer(last);
		}
		catch (std::exception const&)
		{
			threw = true;
		}
		require(threw, "applyDeleteLayer did not reject an invalid plan");
		require(building.getLayerCount() == 2, "A rejected layer deletion changed the layer count");

		auto const missing = building.planDeleteLayer(7);
		require(!missing.valid && !missing.diagnostic.empty(),
			"Deleting a nonexistent layer was not rejected");

		core::Building emptyBack("Empty back", 4, 2);
		emptyBack.addCorridor(0, 0, 4);
		emptyBack.addLayer();
		emptyBack.finishBuild();
		emptyBack.pauseSimulation();
		auto const back = emptyBack.planDeleteLayer(2);
		require(back.valid, ("Deleting the empty back-most layer was rejected: " + back.diagnostic).c_str());
		require(back.requiresConfirmation() && !back.consequences.empty(),
			"An empty layer deletion offered nothing to confirm");
		require(emptyBack.applyDeleteLayer(back) && emptyBack.getLayerCount() == 2,
			"The empty back-most layer was not removed");
		require(emptyBack.getNumSectors() == 1,
			"Deleting an empty layer changed the Sector count");
	}

	void locationEditsArePlannedAndAppliedAtomically()
	{
		core::Building building("Editable", 8, 3);
		auto room = building.addRoom("Room", 0, 0, 0, 5, 2);
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

	void editedShuttleRoundTripsWithoutSchemaChanges()
	{
		core::Building building("Serializable Shuttle", 32, 3);
		building.addCorridor(0, 0, 31);
		building.addCorridor(1, 0, 31);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101;
		options.minimumDwellSeconds = 1.25f;
		options.maximumBoardingSeconds = 4.5f;
		auto created = building.addShuttle(0, 0, 27, options);
		building.finishBuild();
		building.pauseSimulation();
		auto move = building.planResizeShuttle(created.shuttle.sector->getIndex(), 1, 1, 27);
		require(move.valid, "Serializable Shuttle move was rejected");
		auto shuttleIndex = building.applyShuttleEdit(move);
		auto add = building.planAddShuttleStop(shuttleIndex, 9);
		require(add.valid, "Serializable Shuttle stop addition was rejected");
		shuttleIndex = building.applyShuttleEdit(add);

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		auto yaml = writer->getSerializedString();
		require(yaml.find("type: shuttle") != std::string::npos
			&& yaml.find("numCars: 2") != std::string::npos
			&& yaml.find("capacityPerCarriage: 2") != std::string::npos
			&& yaml.find("doorMask: 5") != std::string::npos
			&& yaml.find("allowPartialLandings: false") != std::string::npos,
			"Edited Shuttle did not use the existing explicit YAML schema");

		core::Building loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Edited Shuttle YAML did not deserialize");
		auto shuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(loaded.getSector(shuttleIndex));
		require(shuttle && shuttle->getCellX() == 1 && shuttle->getCellY() == 1
			&& shuttle->getCellsWide() == 27 && shuttle->getNumStops() == 3,
			"Edited Shuttle geometry or stops did not round-trip");
		core::Building::CreateShuttleOptions loadedOptions{};
		require(loaded.getShuttleOptions(shuttle->getShuttle().get(), loadedOptions)
			&& loadedOptions.numCars == 2 && loadedOptions.carWidth == 3
			&& loadedOptions.capacity == 2 && loadedOptions.doorMask == 0b101
			&& std::abs(loadedOptions.minimumDwellSeconds - 1.25f) < 0.001f
			&& std::abs(loadedOptions.maximumBoardingSeconds - 4.5f) < 0.001f
			&& !loadedOptions.allowPartialLandings,
			"Edited Shuttle configuration did not round-trip");
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
		auto room = building.addRoom("Back room", 1, 0, 4, 3, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[1] = true;

		auto first = building.addSectorDoor(0, 5, options);
		auto second = building.addSectorDoor(0, 6, options);
		require(std::abs(controlCenterX(first.controls[1]) - 5.0f) < 0.0001f
			&& std::abs(controlCenterX(second.controls[1]) - 6.0f) < 0.0001f,
			"Adjacent Citadel-style Door controls did not choose distinct X positions");
		auto standardY = CORE_BUTTON_Y_OFFSET
			+ first.controls[1].sector->getObject(first.controls[1].index)
				->_getObject()->getSize().y * 0.5f;
		require(std::abs(controlCenterY(first.controls[1]) - standardY) < 0.0001f
			&& std::abs(controlCenterY(second.controls[1]) - standardY) < 0.0001f,
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
		fallback.addRoom("Narrow back room", 1, 0, 0, 2, 1);
		auto left = fallback.addSectorDoor(0, 0, options);
		auto right = fallback.addSectorDoor(0, 1, options);
		require(std::abs(controlCenterX(left.controls[1])
				- controlCenterX(right.controls[1])) < 0.0001f
			&& std::abs(controlCenterY(left.controls[1])
				- controlCenterY(right.controls[1])) > 0.049f,
			"Unavoidable same-X controls did not use the height fallback");
	}

	void platformLiftStopDurationRoundTrips()
	{
		core::Building original("Serializable PlatformLift", 7, 4);
		auto room = original.addRoom("Platform room", 0, 0, 0, 6, 3);
		for (uint32_t deck = 1; deck <= 2; ++deck)
		{
			original.addSectorWalkway(room, deck, 2);
			original.addSectorWalkway(room, deck, 3);
		}
		core::Building::CreateLiftOptions options;
		options.stopOffsets = { 0, 1, 2 };
		options.platformStopDurationSeconds = 3.5f;
		auto created = original.addSectorPlatformLift(room, 0, 2, options);
		original.finishBuild();

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto yaml = writer->getSerializedString();
		require(yaml.find("stopDurationSeconds: 3.5") != std::string::npos
			&& yaml.find("minimumDwellSeconds") == std::string::npos
			&& yaml.find("maximumBoardingSeconds") == std::string::npos,
			"PlatformLift did not serialize its single stop timer");

		core::Building loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "PlatformLift YAML did not deserialize");
		core::Building::CreateLiftOptions loadedOptions;
		require(loaded.getPlatformLiftOptions(created.lift.sector->getIndex(), created.lift.index,
				loadedOptions)
			&& std::abs(loadedOptions.platformStopDurationSeconds - 3.5f) < 0.001f,
			"PlatformLift stop timer did not round-trip");

		core::Building::CreateLiftOptions defaults;
		require(std::abs(defaults.platformStopDurationSeconds
			- CORE_PLATFORM_LIFT_STOP_DURATION) < 0.001f,
			"PlatformLift stop timer default is not the Defines.h value");
	}

	void enclosedLiftsSupportMultiDeckRooms()
	{
		core::Building building("Room lift", 16, 3);
		auto room = building.addRoom("Lift Hall", 0, 0, 0, 16, 3);
		for (uint32_t deck = 1; deck < 3; ++deck)
			for (uint32_t x = 0; x < 16; ++x)
				building.addSectorWalkway(room, deck, x);

		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		options.decksHigh = 3;
		options.stopOffsets = { 0, 1, 2 };
		auto created = building.addLift(0, 8, options);
		building.addSectorMarker(room, 1, 0.5f);
		building.addSectorMarker(room, 2, 15.5f);
		building.finishBuild();

		auto lift = std::dynamic_pointer_cast<const core::LiftTransit>(created.lift.sector);
		require(lift && lift->getNumStops() == 3 && created.doors.size() == 3,
			"An enclosed Lift could not connect Ground and Walkways in one Fore-layer Room");
	}

	void stairwellSectorsAreCanvasSelectable()
	{
		require(isCanvasSelectableSectorType(core::SectorType::Stairwell),
			"Placed Stairwells cannot be selected by the canvas hit-test");
		require(!shouldDrawCanvasSectorEditOverlay(1, 0),
			"A selected Back-layer Stairwell is overlaid in front of the Fore layer");
		require(!shouldRenderStairwellGeometry(1, false),
			"Hidden Back-layer Stairwell geometry is rendered over the Fore layer");
		require(shouldRenderStairwellGeometry(1, true)
			&& shouldRenderStairwellGeometry(0, true),
			"Stairwell geometry was suppressed from a visible or clipped Fore-layer pass");
		require(!shouldRenderSectorAgents(core::SectorType::Stairwell, 1, false)
			&& shouldRenderSectorAgents(core::SectorType::Stairwell, 0, true)
			&& shouldRenderSectorAgents(core::SectorType::Stairwell, 1, true),
			"Stairwell Agents do not obey the Stairwell's foreground aperture clipping");
		core::Stairwell leftStairwell(0, 0, 3, CORE_SIDE_LEFT);
		core::Stairwell rightStairwell(0, 0, 3, CORE_SIDE_RIGHT);
		auto left = leftStairwell.getDeckPath(0);
		auto right = rightStairwell.getDeckPath(0);
		for (size_t i = 0; i < left.size(); ++i)
			require(std::abs(left[i].x + right[i].x - 2.0f) < 0.0001f
				&& left[i].y == right[i].y,
				"Right-mounted Stairwell path is not mirrored horizontally");
		require(left[0].x == 1.0f && left[0].y == 0.0f
			&& std::abs(left[1].x - 1.666f) < 0.0001f && left[1].y == 0.25f
			&& std::abs(left[2].x - 0.334f) < 0.0001f && left[2].y == 0.75f
			&& left[3].x == 1.0f && left[3].y == 1.0f,
			"Stairwell primitive endpoints do not match its path vertices");
		auto nextDeck = leftStairwell.getDeckPath(1);
		require(left[3] == nextDeck[0],
			"Adjacent Stairwell diagonal paths do not share a deck endpoint");
	}

	void staircasesConnectAdjacentCorridorsAndRoundTrip()
	{
		require(isCanvasSelectableSectorType(core::SectorType::Staircase),
			"Placed Staircases cannot be selected by the canvas hit-test");
		require(shouldRenderStaircaseAfterSector(core::SectorType::Location)
			&& !shouldRenderStaircaseAfterSector(core::SectorType::Staircase),
			"Staircases are not ordered after Fore-layer Rooms and Corridors");
		require(!shouldRenderSectorAgents(core::SectorType::Staircase, 1, false)
			&& shouldRenderSectorAgents(core::SectorType::Staircase, 0, true),
			"Staircase Agents do not obey corridor clipping");

		core::Staircase right(0, 0, 4, CORE_SIDE_RIGHT);
		core::Staircase left(0, 0, 4, CORE_SIDE_LEFT);
		auto rightPath = right.getPath();
		auto leftPath = left.getPath();
		require(right.getStepCount() == 32 && rightPath[0].x == 0.0f
			&& rightPath[1].x == 4.0f && leftPath[0].x == 4.0f
			&& leftPath[1].x == 0.0f,
			"Staircase direction, endpoints, or width-based step count is incorrect");
		float const pathLength = rightPath[0].distanceTo(rightPath[1]);
		core::Staircase upEscalator(0, 0, 4, CORE_SIDE_RIGHT, 1.0f);
		core::Staircase downEscalator(0, 0, 4, CORE_SIDE_RIGHT, -1.0f);
		upEscalator.update(pathLength * 0.25f);
		downEscalator.update(pathLength * 0.25f);
		right.update(pathLength);
		require(std::abs(upEscalator.getAnimationPhase() - 0.25f) < 0.001f
			&& std::abs(downEscalator.getAnimationPhase() - 0.75f) < 0.001f
			&& right.getAnimationPhase() == 0.0f,
			"Escalator step animation does not follow its signed world speed");

		core::Building building("Staircase", 6, 3);
		building.addCorridor(0, 0, 1);
		building.addCorridor(0, 3, 1);
		building.addCorridor(1, 0, 1);
		building.addCorridor(1, 3, 1);
		std::string diagnostic;
		require(!building.canAddStaircase(0, 0, 1, CORE_SIDE_RIGHT, &diagnostic),
			"A one-cell Staircase was accepted");
		require(building.canAddStaircase(0, 0, 4, CORE_SIDE_RIGHT, &diagnostic),
			"A valid Staircase between endpoint Corridors was rejected");
		auto index = building.addStaircase(0, 0, 4, CORE_SIDE_RIGHT, 1.25f);
		building.finishBuild();
		auto transit = std::dynamic_pointer_cast<const core::StaircaseTransit>(building.getSector(index));
		require(transit && transit->getCellsWide() == 4 && transit->getDecksHigh() == 2,
			"Staircase Transit has the wrong footprint");
		core::Building::CreateStaircaseOptions options;
		require(building.getStaircaseOptions(index, options) && options.cellsWide == 4
			&& options.riseSide == CORE_SIDE_RIGHT && std::abs(options.speed - 1.25f) < 0.001f,
			"Staircase authored options were not retained");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData); writer->serialize();
		auto yaml = writer->getSerializedString();
		require(yaml.find("type: staircase") != std::string::npos
			&& yaml.find("speed: 1.25") != std::string::npos,
			"Staircase speed was not serialized");
		core::Building loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml); reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Staircase YAML did not deserialize");
		auto loadedTransit = std::dynamic_pointer_cast<const core::StaircaseTransit>(loaded.getSector(index));
		require(loadedTransit && loadedTransit->getRiseSide() == CORE_SIDE_RIGHT
			&& std::abs(loadedTransit->getStaircase()->getSpeed() - 1.25f) < 0.001f,
			"Staircase did not round-trip through YAML");

		auto escalatorEdge = std::find_if(building.getGraph()->getEdges().begin(),
			building.getGraph()->getEdges().end(), [](auto const& edge)
			{ return edge->getType() == core::EdgeType::Staircase; });
		require(escalatorEdge != building.getGraph()->getEdges().end(),
			"Escalator traversal edge was not created");
		auto edge = *escalatorEdge;
		auto low = edge->getVertex(0)->getPosition().y < edge->getVertex(1)->getPosition().y
			? edge->getVertex(0) : edge->getVertex(1);
		auto high = low == edge->getVertex(0) ? edge->getVertex(1) : edge->getVertex(0);
		require(edge->isTraversable(high, nullptr) && !edge->isTraversable(low, nullptr)
			&& std::isfinite(edge->getWeight(high, nullptr, true))
			&& !std::isfinite(edge->getWeight(low, nullptr, true))
			&& std::abs(edge->getTraversalSpeed(nullptr) - 1.25f) < 0.001f,
			"Positive-speed Escalator is not one-way upward at its configured speed");

		building.pauseSimulation();
		auto flip = building.planResizeStaircase(index, 0, 0, { 4, CORE_SIDE_LEFT, -0.75f });
		require(flip.valid, "A valid Staircase direction flip was rejected");
		index = building.applyStaircaseEdit(flip);
		transit = std::dynamic_pointer_cast<const core::StaircaseTransit>(building.getSector(index));
		require(transit && transit->getRiseSide() == CORE_SIDE_LEFT
			&& std::abs(transit->getStaircase()->getSpeed() + 0.75f) < 0.001f,
			"Staircase direction or Escalator speed was not edited");
		escalatorEdge = std::find_if(building.getGraph()->getEdges().begin(),
			building.getGraph()->getEdges().end(), [](auto const& candidate)
			{ return candidate->getType() == core::EdgeType::Staircase; });
		edge = *escalatorEdge;
		low = edge->getVertex(0)->getPosition().y < edge->getVertex(1)->getPosition().y
			? edge->getVertex(0) : edge->getVertex(1);
		high = low == edge->getVertex(0) ? edge->getVertex(1) : edge->getVertex(0);
		require(edge->isTraversable(low, nullptr) && !edge->isTraversable(high, nullptr)
			&& std::abs(edge->getTraversalSpeed(nullptr) - 0.75f) < 0.001f,
			"Negative-speed Escalator is not one-way downward at its configured speed");
		auto removal = building.planRemoveStaircase(index);
		require(removal.valid && removal.requiresConfirmation(),
			"Staircase deletion was not planned as a confirmed edit");
		require(building.applyStaircaseEdit(removal) == ~0u,
			"Staircase deletion did not return the removed-sector sentinel");
		require(!static_cast<core::Building const&>(building).getLayer(1)
			->getCellDefinition(0, 0).occupied(),
			"Deleted Staircase still occupies the Back layer");
	}

	void laddersCanBeValidatedEditedAndDeleted()
	{
		require(isCanvasSelectableSectorType(core::SectorType::Ladder),
			"Placed Ladders cannot be selected by the canvas hit-test");
		require(!shouldRenderLadderGeometry(1, false),
			"Hidden Back-layer Ladder geometry bypasses Fore-layer corridor clipping");
		require(shouldRenderLadderGeometry(1, true)
			&& shouldRenderLadderGeometry(0, true),
			"Ladder geometry was suppressed from a visible or clipped Fore-layer pass");
		require(shouldRenderForeContentAfterTransit(core::SectorType::Ladder),
			"Clipped sector Ladder geometry is rendered in front of its Location contents");
		require(!shouldRenderLadderGeometryAfterSectorContents(),
			"Sector Ladder geometry is redrawn in front of its occupying Agents");
		require(!shouldRenderSectorAgents(core::SectorType::Ladder, 1, false)
			&& shouldRenderSectorAgents(core::SectorType::Ladder, 0, false)
			&& shouldRenderSectorAgents(core::SectorType::Ladder, 1, true),
			"Sector Ladder Agents do not obey the Ladder's foreground aperture clipping");
		core::Building edgeBuilding("Edge Ladder controls", 5, 3);
		edgeBuilding.addCorridor(0, 0, 5);
		edgeBuilding.addCorridor(2, 0, 5);
		auto edgeLadder = edgeBuilding.addLadder(0, 4, { 3, true, true });
		auto interiorLadder = edgeBuilding.addLadder(0, 0, { 3, true, true });
		auto retractedLadder = edgeBuilding.addLadder(0, 2, { 3, true, false });
		core::Vector2 retractedMin, retractedMax;
		std::static_pointer_cast<const core::LadderTransit>(retractedLadder.ladder.sector)
			->getLadder()->getCurrentShape(retractedMin, retractedMax);
		require(std::abs((retractedMax.y - retractedMin.y) - 0.2f) < 0.0001f,
			"Retracted Ladders were not rendered at the minimum 0.2 length");
		require(std::abs(controlCenterX(edgeLadder.controls[CORE_LEVEL_LOW]) - 4.2f) < 0.0001f
			&& std::abs(controlCenterX(edgeLadder.controls[CORE_LEVEL_HIGH]) - 4.2f) < 0.0001f,
			"Left-side Ladder controls were not placed at the cell's 0.2 offset");
		require(std::abs(controlCenterX(interiorLadder.controls[CORE_LEVEL_LOW]) - 0.8f) < 0.0001f
			&& std::abs(controlCenterX(interiorLadder.controls[CORE_LEVEL_HIGH]) - 0.8f) < 0.0001f,
			"Right-side Ladder controls were not placed at the cell's 0.8 offset");

		core::Building building("Ladder editing", 10, 5);
		std::vector<uint32_t> corridors;
		for (uint32_t y = 0; y < 5; ++y) corridors.push_back(building.addCorridor(y, 0, 10));
		std::string diagnostic;
		require(!building.canAddLadder(0, 1, 1, &diagnostic)
			&& diagnostic.find("at least two") != std::string::npos,
			"Ladder placement accepted a one-deck footprint");
		require(building.canAddLadder(0, 1, 3, &diagnostic),
			"Valid Ladder placement was rejected");
		auto created = building.addLadder(0, 1, { 3, false, true });
		building.finishBuild();
		building.pauseSimulation();
		auto agentId = building.createAgent("Ladder user", created.ladder.sector->getIndex(), 1, 0.5f);
		auto originalAgentPosition = building.lookupAgent(agentId).entity->getGlobalPosition();

		core::Building::CreateLadderOptions edited{ 3, true, false, 3 };
		auto move = building.planResizeLadder(created.ladder.sector->getIndex(), 4, 1, edited);
		require(move.valid && move.move, "Valid Ladder move was not planned");
		auto movedIndex = building.applyLadderEdit(move);
		auto ladder = std::dynamic_pointer_cast<const core::LadderTransit>(building.getSector(movedIndex));
		require(ladder && ladder->getCellX() == 4 && ladder->getCellY() == 1
			&& ladder->getDecksHigh() == 3,
			"Ladder geometry was not edited");
		auto movedAgent = building.lookupAgent(agentId).entity;
		require(movedAgent
			&& std::abs(movedAgent->getGlobalPosition().x - originalAgentPosition.x - 3.0f) < 0.001f
			&& std::abs(movedAgent->getGlobalPosition().y - originalAgentPosition.y - 1.0f) < 0.001f,
			"An occupying Agent did not move with the Ladder");
		core::Building::CreateLadderOptions loaded{};
		require(building.getLadderOptions(movedIndex, loaded) && loaded.extensible
			&& !loaded.startExtended && loaded.directionalBatchLimit == 3,
			"Ladder configuration was not retained");
		auto countControls = [&](uint32_t sectorIndex)
		{
			uint32_t count = 0;
			auto sector = building.getSector(sectorIndex);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (sector->getObject(i)->getObjectType() == core::SectorObjectType::InteractionPoint)
					++count;
			return count;
		};
		require(countControls(corridors[1]) == 1 && countControls(corridors[3]) == 1,
			"Enabling Ladder extensibility did not create both endpoint controls");

		auto unsupportedLocation = building.planResizeLocation(corridors[1], 0, 1, 3, 1);
		require(!unsupportedLocation.valid
			&& unsupportedLocation.diagnostic.find("Ladder") != std::string::npos,
			"A Location edit was allowed to invalidate a Ladder endpoint");
		auto blocked = building.planResizeLadder(movedIndex, 10, 1, edited);
		require(!blocked.valid, "Out-of-bounds Ladder edit was accepted");
		auto removal = building.planRemoveLadder(movedIndex);
		require(removal.valid && removal.requiresConfirmation(),
			"Ladder deletion was not planned as a confirmed edit");
		require(building.applyLadderEdit(removal) == ~0u,
			"Ladder deletion did not return the removed-sector sentinel");
		require(!static_cast<core::Building const&>(building).getLayer(1)
			->getCellDefinition(4, 1).occupied(),
			"Deleted Ladder still occupies the Back layer");
	}

	void stairwellsCanBeValidatedEditedAndDeleted()
	{
		core::Building building("Stairwell editing", 10, 5);
		for (uint32_t y = 0; y < 5; ++y) building.addCorridor(y, 0, 10);
		std::string diagnostic;
		require(!building.canAddStairwell(0, 1, 1, &diagnostic)
			&& diagnostic.find("at least two") != std::string::npos,
			"Stairwell placement accepted a one-deck footprint");
		require(building.canAddStairwell(0, 1, 3, &diagnostic),
			"Valid Stairwell placement was rejected");
		auto created = building.addStairwell(0, 1,
			core::Building::CreateStairwellOptions{ 3, CORE_SIDE_LEFT });
		building.finishBuild();
		building.pauseSimulation();
		auto agentId = building.createAgent("Stair user", created.sectorIndex, 1, 1.0f);
		auto originalAgentPosition = building.lookupAgent(agentId).entity->getGlobalPosition();

		core::Building::CreateStairwellOptions edited{ 3, CORE_SIDE_RIGHT, 2, 3 };
		auto move = building.planResizeStairwell(created.sectorIndex, 4, 1, edited);
		require(move.valid && move.move, "Valid Stairwell move was not planned");
		auto movedIndex = building.applyStairwellEdit(move);
		auto stairwell = std::dynamic_pointer_cast<const core::StairwellTransit>(
			building.getSector(movedIndex));
		require(stairwell && stairwell->getCellX() == 4 && stairwell->getCellY() == 1
			&& stairwell->getDecksHigh() == 3 && stairwell->getMountSide() == CORE_SIDE_RIGHT,
			"Stairwell geometry or mounting side was not edited");
		auto movedAgent = building.lookupAgent(agentId).entity;
		require(movedAgent
			&& std::abs(movedAgent->getGlobalPosition().x - originalAgentPosition.x - 3.0f) < 0.001f
			&& std::abs(movedAgent->getGlobalPosition().y - originalAgentPosition.y - 1.0f) < 0.001f,
			"An occupying Agent did not move with the Stairwell");
		core::Building::CreateStairwellOptions loaded{};
		require(building.getStairwellOptions(movedIndex, loaded)
			&& loaded.directionalCapacity == 2 && loaded.directionalBatchLimit == 3,
			"Stairwell coordination properties were not retained");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		core::Building replayed("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(replayed.deserialize(*reader, workData), "Edited Stairwell YAML did not deserialize");
		core::Building::CreateStairwellOptions replayedOptions{};
		auto replayedStairwell = std::dynamic_pointer_cast<const core::StairwellTransit>(
			replayed.getSector(movedIndex));
		require(replayedStairwell && replayedStairwell->getCellX() == 4
			&& replayedStairwell->getCellY() == 1
			&& replayed.getStairwellOptions(movedIndex, replayedOptions)
			&& replayedOptions.mountSide == CORE_SIDE_RIGHT
			&& replayedOptions.directionalCapacity == 2
			&& replayedOptions.directionalBatchLimit == 3,
			"Edited Stairwell did not round-trip through YAML");

		auto blocked = building.planResizeStairwell(movedIndex, 9, 1, edited);
		require(!blocked.valid, "Out-of-bounds Stairwell edit was accepted");
		auto removal = building.planRemoveStairwell(movedIndex);
		require(removal.valid && removal.requiresConfirmation(),
			"Stairwell deletion was not planned as a confirmed edit");
		require(building.applyStairwellEdit(removal) == ~0u,
			"Stairwell deletion did not return the removed-sector sentinel");
		require(!static_cast<core::Building const&>(building).getLayer(1)
			->getCellDefinition(4, 1).occupied(),
			"Deleted Stairwell still occupies the Back layer");
	}

	void bulkheadDoorsSupportIndependentObjectEditing()
	{
		core::Building building("Bulkhead editor", 7, 2);
		auto const left = building.addRoom("Left", 0, 0, 0, 2, 1);
		building.addRoom("Middle", 0, 0, 2, 2, 1);
		building.addRoom("Right", 0, 0, 4, 2, 1);
		std::string diagnostic;
		require(building.canAddSectorBulkheadDoor(0, 0, 2,
			CORE_SIDE_LEFT, {}, &diagnostic), "valid left-edge Bulkhead Door placement was rejected");
		require(!building.canAddSectorBulkheadDoor(0, 0, 0,
			CORE_SIDE_LEFT, {}, &diagnostic), "Bulkhead Door was accepted at the world edge");
		require(!building.canAddSectorBulkheadDoor(0, 0, 1,
			CORE_SIDE_LEFT, {}, &diagnostic), "Bulkhead Door was accepted inside one Location");

		auto created = building.addSectorBulkheadDoor(0, 0, 2, CORE_SIDE_LEFT);
		std::shared_ptr<const core::SectorObject> object =
			created.door.sector->getObject(created.door.index);
		require(object && object->getObjectType() == core::SectorObjectType::BulkheadDoor
			&& object->getCellX() + 1 == 2,
			"Bulkhead Door was not created on the selected cell's left edge");
		uint32_t ownedControls = 0;
		for (auto const& sector : building.getSectors(0))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (building.isBulkheadDoorOwnedControl(sector->getObject(i))) ++ownedControls;
		require(ownedControls == 2, "Bulkhead Door controls were not recognized as managed objects");

		core::Building::CreateBulkheadDoorOptions options;
		require(building.getSectorBulkheadDoorOptions(left, created.door.index, options)
			&& options.controls[0] && options.controls[1]
			&& options.activationMode == core::DoorActivationMode::RemoteControlled,
			"Bulkhead Door authored options could not be read");
		building.finishBuild();
		building.pauseSimulation();
		options.controls[0] = options.controls[1] = false;
		options.activationMode = core::DoorActivationMode::Manual;
		options.holdOpenSeconds = 3.0f;
		options.crossingLanes = 1;
		object = building.applySectorBulkheadDoorOptions(left, created.door.index, options);
		require(object && object->getCellX() + 1 == 2,
			"Bulkhead Door settings edit lost the selected object");

		auto owner = object->getSector();
		uint32_t objectIndex = ~0u;
		for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			if (owner->getObject(i) == object) { objectIndex = i; break; }
		auto plan = building.planMoveSectorObject(owner->getIndex(), objectIndex, 4, 0);
		require(plan.valid, "Bulkhead Door move to another left-edge boundary was rejected");
		object = building.applyObjectMove(plan);
		require(object && object->getCellX() + 1 == 4,
			"Bulkhead Door move did not use the target cell's left edge");
		owner = object->getSector(); objectIndex = ~0u;
		for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			if (owner->getObject(i) == object) { objectIndex = i; break; }
		require(building.getSectorBulkheadDoorOptions(owner->getIndex(), objectIndex, options)
			&& options.activationMode == core::DoorActivationMode::Manual
			&& !options.controls[0] && !options.controls[1]
			&& std::abs(options.holdOpenSeconds - 3.0f) < 0.0001f
			&& options.crossingLanes == 1,
			"Bulkhead Door move did not preserve authored settings");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData); writer->serialize();
		core::Building loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Bulkhead Door building did not round-trip");
		auto loadedLeft = loaded.getSectorAtPosition(0, 3.5f, 0.5f);
		objectIndex = ~0u;
		for (uint32_t i = 0; loadedLeft && i < loadedLeft->getNumObjects(); ++i)
		{
			auto candidate = loadedLeft->getObject(i);
			if (candidate && candidate->getObjectType() == core::SectorObjectType::BulkheadDoor)
				{ objectIndex = i; break; }
		}
		require(loadedLeft && objectIndex != ~0u
			&& loaded.getSectorBulkheadDoorOptions(loadedLeft->getIndex(), objectIndex, options),
			"Bulkhead Door authored settings did not round-trip");
		loaded.pauseSimulation();
		require(loaded.removeSectorBulkheadDoor(loadedLeft->getIndex(), objectIndex),
			"Bulkhead Door could not be deleted independently");
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

void layerHelperApiIsConsistentWithTwoLayerConstants()
{
	require(core::isFrontMostLayer(0), "fore layer is not reported as front-most");
	require(!core::isFrontMostLayer(1), "back layer reported as front-most");
	require(core::isBackMostLayer(1), "back layer is not reported as back-most");
	require(!core::isBackMostLayer(0), "fore layer reported as back-most");
	require(core::layerInFront(1) == 0, "layerInFront(back) did not return fore");
	require(core::layerBehind(0) == 1, "layerBehind(fore) did not return back");
	require(CORE_MAX_LAYERS == 256, "CORE_MAX_LAYERS is not 256");
}

void runSerializationSmokeChecks()
{
	layerHelperApiIsConsistentWithTwoLayerConstants();
	stringYamlRoundTripsPrimitiveValues();
	fileYamlRoundTrips();
	malformedValuesAndInvalidUsageThrowUsefulErrors();
	buildingRoundTripsAuthoredStateAndAgents();
	platformLiftStopDurationRoundTrips();
	legacyBuildingYamlStillLoads();
	legacyVersion3BuildingYamlStillLoadsWithDefaultLayers();
	layerFieldsAcceptLegacyNamesAndIndices();
	buildingLayerNamesRoundTrip();
	addedLayersAppendToTheBackAndRoundTrip();
	layerCountIsCappedAtCoreMaxLayers();
	deletingAMiddleLayerCompactsTheLayersAboveIt();
	deletingTheFrontLayerRemovesTransitsOneLayerBehind();
	layerDeletionKeepsAtLeastTwoLayers();
	locationEditsArePlannedAndAppliedAtomically();
	editedShuttleRoundTripsWithoutSchemaChanges();
	enclosedLiftsSupportMultiDeckRooms();
	stairwellSectorsAreCanvasSelectable();
	staircasesConnectAdjacentCorridorsAndRoundTrip();
	laddersCanBeValidatedEditedAndDeleted();
	stairwellsCanBeValidatedEditedAndDeleted();
	physicalControlsPreferDistinctWallPositions();
	bulkheadDoorsSupportIndependentObjectEditing();
	recentFilesPersistAcrossStartup();
	serializableTracksModificationState();
}
