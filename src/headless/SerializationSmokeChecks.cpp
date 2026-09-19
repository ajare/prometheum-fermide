#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "RecentFiles.h"
#include "core/Exceptions.h"
#include "Render.h"
#include "UI.h"

#include "core/Graph.h"
#include "core/Building.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
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

	// #62: a late save write failure must report failure and leave the previous
	// save file intact, with no temporary file left behind.
	void lateWriteFailurePreservesThePreviousSaveFile()
	{
		namespace filesystem = std::filesystem;
		filesystem::path const directory = filesystem::temp_directory_path() / "pf-save-transaction-smoke";
		std::error_code error;
		filesystem::remove_all(directory, error);
		filesystem::create_directories(directory);
		struct DirectoryCleanup
		{
			filesystem::path path;
			~DirectoryCleanup()
			{
				std::error_code ignored;
				filesystem::remove_all(path, ignored);
			}
		} cleanup{ directory };
		filesystem::path const destination = directory / "building.yaml";

		auto const originalContents = std::string("original save contents\n");
		{
			std::ofstream original(destination, std::ios::binary);
			original << originalContents;
		}

		auto makeWriter = [&destination]()
		{
			auto writer = core::YamlSerializer::toFile(destination.string());
			writer->beginMap("");
			writer->writeString("payload", std::string(256 * 1024, 'x'));
			writer->endMap();
			return writer;
		};
		auto readFile = [&destination]()
		{
			std::ifstream in(destination, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		};
		auto countRegularFiles = [&directory]()
		{
			int count = 0;
			for (auto const& entry : filesystem::directory_iterator(directory))
			{
				if (entry.is_regular_file()) ++count;
			}
			return count;
		};

		core::YamlSerializer::setWriteFailureAfterBytesForTesting(4096);
		bool reportedFailure = false;
		try
		{
			makeWriter()->serialize();
		}
		catch (core::SerializationException const&)
		{
			reportedFailure = true;
		}
		core::YamlSerializer::setWriteFailureAfterBytesForTesting(0);

		require(reportedFailure, "injected late write failure did not report a save error");
		require(readFile() == originalContents, "failed save destroyed the previous save file");
		require(countRegularFiles() == 1, "failed save left a temporary file behind");

		// A successful save installs the new contents and also leaves no
		// temporary file behind.
		makeWriter()->serialize();
		require(readFile().find(std::string(1024, 'x')) != std::string::npos,
			"successful save did not install the new contents");
		require(countRegularFiles() == 1, "successful save left a temporary file behind");
	}

	// #63: a failed save must not clear the Building's unsaved-changes state.
	// The clean-state transition may only happen after the file write has fully
	// succeeded, so Save stays available and closing still prompts for unsaved
	// changes after any open, write, flush, close, or replacement error.
	void failedSavePreservesUnsavedChangesState()
	{
		namespace filesystem = std::filesystem;
		filesystem::path const directory = filesystem::temp_directory_path() / "pf-dirty-save-smoke";
		std::error_code error;
		filesystem::remove_all(directory, error);
		filesystem::create_directories(directory);
		struct DirectoryCleanup
		{
			filesystem::path path;
			~DirectoryCleanup()
			{
				std::error_code ignored;
				filesystem::remove_all(path, ignored);
			}
		} cleanup{ directory };
		filesystem::path const destination = directory / "building.yaml";

		core::Building building("Dirty save", 8, 2);
		building.addRoom("Fore room", 0, 0, 0, 7, 1);
		building.addRoom("Back room", 1, 0, 0, 7, 1);
		building.finishBuild();
		// Enough Agents that the YAML exceeds the injected failure threshold,
		// so the failure lands mid-write rather than at open.
		for (int i = 0; i < 200; ++i)
		{
			building.createAgent("Agent keeping the document dirty " + std::to_string(i), 0, 0, 0.5f);
		}

		// A fully successful save marks the document clean.
		building.saveTo(destination.string());
		require(!building.isModified(), "successful save did not clear the unsaved-changes state");

		// Edit again, then fail the save mid-write.
		building.markModified();
		require(building.isModified(), "Building did not become dirty after an edit");
		core::YamlSerializer::setWriteFailureAfterBytesForTesting(4096);
		bool reportedFailure = false;
		try
		{
			building.saveTo(destination.string());
		}
		catch (core::SerializationException const&)
		{
			reportedFailure = true;
		}
		core::YamlSerializer::setWriteFailureAfterBytesForTesting(0);
		require(reportedFailure, "injected write failure did not report a save error");
		require(building.isModified(),
			"failed save cleared the unsaved-changes state; Save would be disabled and "
			"closing would not prompt for unsaved changes");

		// Retrying the save after the failure succeeds and only then goes clean.
		building.saveTo(destination.string());
		require(!building.isModified(), "retry save after failure did not clear the unsaved-changes state");
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
		original.addSectorDoor(0, 0, 3, doorOptions);
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
		require(yaml.find("version: 6") != std::string::npos
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

	// The version 5 writer must not strand the version 4 files already on disk.
	void version4BuildingYamlStillLoads()
	{
		auto const yaml = R"yaml(version: 4
name: Legacy v4
cellsWide: 6
decksHigh: 2
layers: 3
layerNames:
  - Ground
  - Mezzanine
  - Sublevel
construction:
  - type: room
    name: Ground room
    layer: 0
    y: 0
    x: 0
    cellsWide: 3
    decksHigh: 1
    topDeckHeight: 0.9
  - type: room
    name: Deep room
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
		require(loaded.deserialize(*reader, workData), "version 4 Building YAML no longer loads");
		core::Building const& loadedRef = loaded;
		require(loaded.getName() == "Legacy v4"
			&& loaded.getLayerCount() == 3
			&& loadedRef.getNumSectors() == 2
			&& loadedRef.getSector(0)->getLayerIndex() == 0
			&& loadedRef.getSector(1)->getLayerIndex() == 2,
			"version 4 Building YAML did not load into the same shape");
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
		auto const yaml = R"yaml(version: 5
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
		building.addSectorDoor(0, 0, 2);
		// Authored on the front Layer of the 1<->2 pair, so it crosses the Layer the
		// test deletes.
		building.addSectorWindow(1, 0, 5, 1, 1);
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
		building.addLadder(1, 0, 3, { 3, false, true });
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

	// Authored record order carries dependencies: a wall removal has to replay
	// before the Staircase which needs that wall open, and records which point at a
	// Sector must keep pointing at the same Sector after the indices compact.
	void layerDeletionPreservesAuthoredRecordDependencies()
	{
		core::Building building("Dependencies", 16, 3);
		building.addLayer();
		building.addCorridor(0, 0, 7);
		building.addCorridor(1, 3, 7);
		building.addStaircase(1, 0, 4, { 4, CORE_SIDE_RIGHT, 0.4f });
		building.addRoom("Room 1", 0, 1, 10, 4, 2);
		building.addCorridor(2, 14, 2);
		building.removeLocationWall(3, 1, CORE_SIDE_RIGHT);
		building.addStaircase(1, 1, 11, { 3, CORE_SIDE_RIGHT, 0.0f });
		building.finishBuild();
		building.pauseSimulation();

		// Deleting the Transit Layer drops two Sectors ahead of the Room, so the
		// wall removal must follow the Room rather than land on a renumbered Sector.
		auto const middle = building.planDeleteLayer(1);
		require(middle.valid,
			("Dependency-aware layer deletion was rejected: " + middle.diagnostic).c_str());
		require(middle.transitsRemoved == 2, "Dependency test dropped the wrong Transits");
		building.applyDeleteLayer(middle);
		require(building.getNumSectors() == 4, "Dependency test compacted to the wrong Sector count");
		require(building.getSector(2)->getName() == "Room 1",
			"The wall removal did not follow its Room through the Sector compaction");
		require(building.getSector(2)->getEndType(1, CORE_SIDE_RIGHT) != core::SectorEndType::Wall,
			"The removed wall came back after the layer deletion");
		require(building.isTraversalTopologyValid(),
			("Layer deletion left an invalid topology: " + building.getTopologyDiagnostic()).c_str());

		// Deleting the back-most Layer drops nothing, so this exercises the record
		// ordering alone.
		core::Building untouched("Dependencies", 16, 3);
		untouched.addLayer();
		untouched.addCorridor(0, 0, 7);
		untouched.addCorridor(1, 3, 7);
		untouched.addStaircase(1, 0, 4, { 4, CORE_SIDE_RIGHT, 0.4f });
		untouched.addRoom("Room 1", 0, 1, 10, 4, 2);
		untouched.addCorridor(2, 14, 2);
		untouched.removeLocationWall(3, 1, CORE_SIDE_RIGHT);
		untouched.addStaircase(1, 1, 11, { 3, CORE_SIDE_RIGHT, 0.0f });
		untouched.finishBuild();
		untouched.pauseSimulation();
		auto const back = untouched.planDeleteLayer(2);
		require(back.valid,
			("Deleting the back-most Layer broke an authored dependency: " + back.diagnostic).c_str());
		untouched.applyDeleteLayer(back);
		require(untouched.getNumSectors() == 6 && untouched.getLayerCount() == 2,
			"Deleting the back-most Layer changed more than the Layer count");
		require(untouched.isTraversalTopologyValid(),
			("Back-most layer deletion left an invalid topology: "
				+ untouched.getTopologyDiagnostic()).c_str());
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
		auto created = building.addShuttle(1, 0, 0, 27, options);
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

		auto first = building.addSectorDoor(0, 0, 5, options);
		auto second = building.addSectorDoor(0, 0, 6, options);
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
		auto left = fallback.addSectorDoor(0, 0, 0, options);
		auto right = fallback.addSectorDoor(0, 0, 1, options);
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
		auto created = building.addLift(1, 0, 8, options);
		building.addSectorMarker(room, 1, 0.5f);
		building.addSectorMarker(room, 2, 15.5f);
		building.finishBuild();

		auto lift = std::dynamic_pointer_cast<const core::LiftTransit>(created.lift.sector);
		require(lift && lift->getNumStops() == 3 && created.doors.size() == 3,
			"An enclosed Lift could not connect Ground and Walkways in one Fore-layer Room");
	}

	void stopDerivingAddLiftRejectsInvalidLayerIndex()
	{
		core::Building building("Lift layer validation", 8, 2);

		bool rejected = false;
		try
		{
			// Layer 0 is the front-most Layer: it has no Layer in front for the landings.
			building.addLift(0, 0, 2, 1, 1);
		}
		catch (core::BuildingException const&)
		{
			rejected = true;
		}
		require(rejected, "The stop-deriving addLift() did not reject the front-most Layer");

		rejected = false;
		try
		{
			building.addLift(2, 0, 2, 1, 1);
		}
		catch (core::BuildingException const&)
		{
			rejected = true;
		}
		require(rejected, "The stop-deriving addLift() did not reject a Layer past the layer count");
	}

	void stairwellSectorsAreCanvasSelectable()
	{
		require(isCanvasSelectableSectorType(core::SectorType::Stairwell),
			"Placed Stairwells cannot be selected by the canvas hit-test");
		require(!shouldDrawCanvasSectorEditOverlay(1, 0),
			"A selected Back-layer Stairwell is overlaid in front of the Fore layer");
		require(!shouldRenderStairwellGeometry(LayerRenderStyle::Wireframe)
			&& !shouldRenderStairwellGeometry(LayerRenderStyle::Hidden),
			"A hidden Layer's Stairwell geometry is rendered over the selected Layer");
		require(shouldRenderStairwellGeometry(LayerRenderStyle::Solid)
			&& shouldRenderStairwellGeometry(LayerRenderStyle::Aperture),
			"Stairwell geometry was suppressed from the selected Layer or an aperture pass");
		require(!shouldRenderSectorAgents(core::SectorType::Stairwell, LayerRenderStyle::Wireframe)
			&& shouldRenderSectorAgents(core::SectorType::Stairwell, LayerRenderStyle::Solid)
			&& shouldRenderSectorAgents(core::SectorType::Stairwell, LayerRenderStyle::Aperture),
			"Stairwell Agents do not obey the Stairwell's aperture clipping");
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
		require(!shouldRenderSectorAgents(core::SectorType::Staircase, LayerRenderStyle::Wireframe)
			&& shouldRenderSectorAgents(core::SectorType::Staircase, LayerRenderStyle::Aperture),
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
		require(!building.canAddStaircase(1, 0, 0, 1, CORE_SIDE_RIGHT, &diagnostic),
			"A one-cell Staircase was accepted");
		require(building.canAddStaircase(1, 0, 0, 4, CORE_SIDE_RIGHT, &diagnostic),
			"A valid Staircase between endpoint Corridors was rejected");
		auto index = building.addStaircase(1, 0, 0, 4, CORE_SIDE_RIGHT, 1.25f);
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
		require(!shouldRenderLadderGeometry(LayerRenderStyle::Wireframe)
			&& !shouldRenderLadderGeometry(LayerRenderStyle::Hidden),
			"A hidden Layer's Ladder geometry bypasses the selected Layer's clipping");
		require(shouldRenderLadderGeometry(LayerRenderStyle::Solid)
			&& shouldRenderLadderGeometry(LayerRenderStyle::Aperture),
			"Ladder geometry was suppressed from the selected Layer or an aperture pass");
		require(shouldRenderForeContentAfterTransit(core::SectorType::Ladder),
			"Clipped sector Ladder geometry is rendered in front of its Location contents");
		require(!shouldRenderLadderGeometryAfterSectorContents(),
			"Sector Ladder geometry is redrawn in front of its occupying Agents");
		require(!shouldRenderSectorAgents(core::SectorType::Ladder, LayerRenderStyle::Wireframe)
			&& shouldRenderSectorAgents(core::SectorType::Ladder, LayerRenderStyle::Aperture)
			&& shouldRenderSectorAgents(core::SectorType::Ladder, LayerRenderStyle::Solid),
			"Sector Ladder Agents do not obey the Ladder's aperture clipping");
		core::Building edgeBuilding("Edge Ladder controls", 5, 3);
		edgeBuilding.addCorridor(0, 0, 5);
		edgeBuilding.addCorridor(2, 0, 5);
		auto edgeLadder = edgeBuilding.addLadder(1, 0, 4, { 3, true, true });
		auto interiorLadder = edgeBuilding.addLadder(1, 0, 0, { 3, true, true });
		auto retractedLadder = edgeBuilding.addLadder(1, 0, 2, { 3, true, false });
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
		require(!building.canAddLadder(1, 0, 1, 1, &diagnostic)
			&& diagnostic.find("at least two") != std::string::npos,
			"Ladder placement accepted a one-deck footprint");
		require(building.canAddLadder(1, 0, 1, 3, &diagnostic),
			"Valid Ladder placement was rejected");
		auto created = building.addLadder(1, 0, 1, { 3, false, true });
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
		require(!building.canAddStairwell(1, 0, 1, 1, &diagnostic)
			&& diagnostic.find("at least two") != std::string::npos,
			"Stairwell placement accepted a one-deck footprint");
		require(building.canAddStairwell(1, 0, 1, 3, &diagnostic),
			"Valid Stairwell placement was rejected");
		auto created = building.addStairwell(1, 0, 1,
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

	// A Transit on the Layer behind the selection is visible only through the
	// apertures the selected Layer's Locations give it. Each Transit type exposes
	// its own aperture geometry, and a Transit with no aperture is not drawn.
	void transitsOnTheLayerBehindAreOnlyDrawnThroughApertures()
	{
		require(!shouldClipTransitToApertures(LayerRenderStyle::Solid),
			"The selected Layer clips its own Transits to apertures");
		require(shouldClipTransitToApertures(LayerRenderStyle::Aperture),
			"A Transit seen through an aperture is drawn unclipped");
		require(!shouldClipTransitToApertures(LayerRenderStyle::Wireframe),
			"The wireframe overlay clips its Transits instead of outlining the whole Layer behind");
		require(!shouldClipTransitToApertures(LayerRenderStyle::Hidden),
			"A hidden Layer is clipped instead of not drawn");

		auto opensInsideLanding = [](TransitAperture const& aperture)
		{
			if (!aperture.location) return false;
			core::Vector2 lo, hi;
			aperture.location->getBounds(lo, hi);
			return aperture.min.x >= lo.x && aperture.max.x <= hi.x
				&& aperture.min.y >= lo.y && aperture.max.y <= hi.y;
		};

		// A Lift opens one doorway per landing, inset from the shaft's cells.
		{
			core::Building building("Lift apertures", 16, 3);
			auto room = building.addRoom("Lift Hall", 0, 0, 0, 16, 3);
			for (uint32_t deck = 1; deck < 3; ++deck)
				for (uint32_t x = 0; x < 16; ++x)
					building.addSectorWalkway(room, deck, x);
			core::Building::CreateLiftOptions options;
			options.cellsWide = 1;
			options.decksHigh = 3;
			options.stopOffsets = { 0, 1, 2 };
			auto created = building.addLift(1, 0, 8, options);
			building.finishBuild();

			auto const transit = created.lift.sector;
			auto const apertures = transitApertures(transit, 0, building.getSectors(0));
			require(apertures.size() == 3,
				"A three-stop Lift does not expose one aperture per landing");
			for (auto const& aperture : apertures)
			{
				require(std::abs((aperture.max.x - aperture.min.x)
						- (1.0f - CORE_LIFT_DOORWAY_BORDER * 2.0f)) < 0.0001f
						&& std::abs((aperture.max.y - aperture.min.y)
							- CORE_LIFT_DOORWAY_HEIGHT) < 0.0001f,
					"A Lift aperture is not its landing doorway");
				require(opensInsideLanding(aperture),
					"A Lift aperture opens outside the Location it lands in");
			}
			require(std::abs(apertures[1].min.y - apertures[0].min.y - 1.0f) < 0.0001f,
				"Lift landing apertures do not step one deck each");
			require(transitApertures(transit, 1, building.getSectors(1)).empty(),
				"A Lift exposes apertures on a Layer it is not directly behind");
		}

		// A Shuttle opens one doorway per carriage door it actually owns, at that
		// Door's own rectangle - not one per landing at the stop's origin cell.
		{
			core::Building building("Shuttle apertures", 32, 3);
			building.addCorridor(0, 0, 31);
			building.addCorridor(1, 0, 31);
			core::Building::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
			options.capacity = 2;
			// Doors on carriage cells 1 and 2, so no doorway sits at a stop's origin
			// cell and the old stop-derived aperture is distinguishable from a real one.
			options.doorMask = 0b110;
			auto created = building.addShuttle(1, 0, 0, 27, options);
			building.finishBuild();

			auto const transit = created.shuttle.sector;
			auto const apertures = transitApertures(transit, 0, building.getSectors(0));

			// Two stops x two carriages x two doors each. The stop's origin cell is
			// not a doorway, so deriving apertures from stops rather than from the
			// thresholds leaves the count wrong as well as the placement.
			require(apertures.size() == 8,
				"A Shuttle does not expose one aperture per carriage doorway it owns");

			// The apertures are exactly the Doors the Shuttle owns on the Layer in
			// front of it, rectangle for rectangle.
			std::vector<std::pair<float, float>> ownedDoorways;
			for (auto const& sector : building.getSectors(0))
				for (uint32_t index = 0; index < sector->getNumObjects(); ++index)
				{
					auto const object = sector->getObject(index);
					if (!object || object->getObjectType() != core::SectorObjectType::Door)
						continue;
					auto const door = std::static_pointer_cast<const core::DoorSectorObject>(
						object)->getDoor();
					if (!door || door->getBackSector() != transit)
						continue;
					core::Vector2 lo, hi;
					door->getFullShape(lo, hi);
					ownedDoorways.push_back({ lo.x, hi.x });
				}
			std::sort(ownedDoorways.begin(), ownedDoorways.end());

			std::vector<std::pair<float, float>> openedDoorways;
			for (auto const& aperture : apertures)
			{
				require(std::abs((aperture.max.x - aperture.min.x)
						- (1.0f - CORE_SHUTTLE_DOORWAY_BORDER * 2.0f)) < 0.0001f
						&& std::abs((aperture.max.y - aperture.min.y)
							- CORE_SHUTTLE_DOORWAY_HEIGHT) < 0.0001f,
					"A Shuttle aperture is not its carriage doorway");
				require(opensInsideLanding(aperture),
					"A Shuttle aperture opens outside the Location it lands in");
				openedDoorways.push_back({ aperture.min.x, aperture.max.x });
			}
			std::sort(openedDoorways.begin(), openedDoorways.end());

			require(openedDoorways == ownedDoorways,
				"A Shuttle's apertures are not the carriage doorways it owns");

			// None of them overlaps a stop's origin cell, which is where the old
			// stop-derived aperture was and where this Shuttle has no doorway.
			constexpr float shuttleX{ 0.0f };
			for (auto const stopOffset : options.stopOffsets)
			{
				auto const cell0 = shuttleX + (float)stopOffset;
				auto const cell1 = cell0 + 1.0f;
				for (auto const& aperture : apertures)
					require(aperture.max.x <= cell0 + 0.0001f || aperture.min.x >= cell1 - 0.0001f,
						"A Shuttle aperture sits at its stop's origin cell rather than at its carriage door");
			}

			require(transitApertures(transit, 1, building.getSectors(1)).empty(),
				"A Shuttle exposes apertures on a Layer it is not directly behind");
		}

		// A Ladder opens the whole of each Location it lands in.
		{
			core::Building building("Ladder apertures", 10, 5);
			for (uint32_t y = 0; y < 5; ++y) building.addCorridor(y, 0, 10);
			auto created = building.addLadder(1, 0, 1, { 3, false, true });
			building.finishBuild();

			auto const transit = created.ladder.sector;
			auto const apertures = transitApertures(transit, 0, building.getSectors(0));
			require(apertures.size() == 2,
				"A Ladder does not expose one aperture per landing Location");
			require(apertures[0].location != apertures[1].location,
				"A Ladder exposes the same Location twice instead of both endpoints");
			for (auto const& aperture : apertures)
			{
				core::Vector2 lo, hi;
				aperture.location->getBounds(lo, hi);
				require(aperture.min.x == lo.x && aperture.min.y == lo.y
					&& aperture.max.x == hi.x && aperture.max.y == hi.y,
					"A Ladder aperture is not the full bounds of its landing Location");
			}
			require(apertures.size() < building.getSectors(0).size(),
				"A Ladder is clipped by every Location rather than only its landings");
		}

		// A Stairwell opens one doorway per deck of its own shaft.
		{
			core::Building building("Stairwell apertures", 10, 5);
			for (uint32_t y = 0; y < 5; ++y) building.addCorridor(y, 0, 10);
			auto created = building.addStairwell(1, 0, 1,
				core::Building::CreateStairwellOptions{ 3, CORE_SIDE_LEFT });
			building.finishBuild();

			auto const transit = building.getSector(created.sectorIndex);
			auto const apertures = transitApertures(transit, 0, building.getSectors(0));
			require(apertures.size() == 3,
				"A three-deck Stairwell does not expose one aperture per deck");
			for (auto const& aperture : apertures)
				require(std::abs((aperture.max.x - aperture.min.x)
						- CORE_STAIRWELL_DOORWAY_WIDTH) < 0.0001f
						&& std::abs((aperture.max.y - aperture.min.y)
							- CORE_STAIRWELL_DOORWAY_HEIGHT) < 0.0001f,
					"A Stairwell deck aperture is not the shaft doorway size");
			require(std::abs(apertures[1].min.y - apertures[0].min.y - 1.0f) < 0.0001f,
				"Stairwell deck apertures do not step one deck each");
		}

		// A Staircase crosses the whole selected Layer, so every Location there
		// clips it.
		{
			core::Building building("Staircase apertures", 6, 3);
			building.addCorridor(0, 0, 1);
			building.addCorridor(0, 3, 1);
			building.addCorridor(1, 0, 1);
			building.addCorridor(1, 3, 1);
			auto const index = building.addStaircase(1, 0, 0, 4, CORE_SIDE_RIGHT, 1.25f);
			building.finishBuild();

			auto const viewSectors = building.getSectors(0);
			uint32_t locations{ 0 };
			for (auto const& sector : viewSectors)
				if (sector->getType() == core::SectorType::Location) ++locations;
			auto const apertures = transitApertures(building.getSector(index), 0, viewSectors);
			require(apertures.size() == locations,
				"A Staircase is not clipped by every Location on the selected Layer");
			for (auto const& aperture : apertures)
				require(opensInsideLanding(aperture),
					"A Staircase aperture is not a Location on the selected Layer");
		}

		// A Location is not a Transit and never exposes an aperture.
		{
			core::Building building("Locations are not Transits", 4, 2);
			auto const corridor = building.addCorridor(0, 0, 4);
			building.finishBuild();
			require(transitApertures(building.getSector(corridor), 0, building.getSectors(0)).empty(),
				"A Location exposes a Transit aperture of its own");
			require(transitApertures(nullptr, 0, building.getSectors(0)).empty(),
				"A missing Sector exposes a Transit aperture");
		}
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

void layerHelperApiIsConsistentWithLayerCount()
{
	require(core::isFrontMostLayer(0), "fore layer is not reported as front-most");
	require(!core::isFrontMostLayer(1), "back layer reported as front-most");
	require(core::isBackMostLayer(1, 2), "back layer is not reported as back-most");
	require(!core::isBackMostLayer(0, 2), "fore layer reported as back-most");
	require(core::isBackMostLayer(2, 3) && !core::isBackMostLayer(1, 3),
		"isBackMostLayer does not follow the Building's Layer count");
	require(core::layerInFront(1) == 0, "layerInFront(back) did not return fore");
	require(core::layerBehind(0) == 1, "layerBehind(fore) did not return back");
	require(CORE_MAX_LAYERS == 256, "CORE_MAX_LAYERS is not 256");
}

// The viewport draws the selected Layer solid and whole, then the Layer directly
// behind it: solid through the apertures the selected Layer gives it, and outlined
// over the selection while the wireframe overlay is on. Every other Layer - in
// front of the selection, or more than one Layer behind it - is hidden.
void onlyTheSelectedLayerAndTheLayerBehindAreDrawn()
{
	require(isLayerDrawn(0, 0, 2), "The selected Layer is not drawn");
	require(isLayerDrawn(1, 0, 2),
		"The Layer directly behind the selection is not drawn");

	require(!isLayerDrawn(0, 1, 2),
		"A Layer in front of the selection is still drawn");

	require(isLayerDrawn(1, 1, 3) && isLayerDrawn(2, 1, 3) && !isLayerDrawn(0, 1, 3),
		"A middle Layer does not draw itself and the Layer directly behind it");

	require(isLayerDrawn(0, 0, 3) && isLayerDrawn(1, 0, 3) && !isLayerDrawn(2, 0, 3),
		"More than one Layer behind the selection is drawn");

	require(isLayerDrawn(2, 2, 3) && !isLayerDrawn(0, 2, 3) && !isLayerDrawn(1, 2, 3),
		"The back-most Layer does not draw itself, or leaves other Layers drawn");

	require(!isLayerDrawn(3, 0, 3) && !isLayerDrawn(0, 3, 3),
		"A Layer index outside the Building's Layers is drawn");

	// The selected Layer is drawn first and whole, the Layer directly behind it
	// next through the selected Layer's apertures, and finally outlined over the
	// selection while the overlay is on.
	{
		auto const passes = renderPasses(0, 3, true);
		require(passes.size() == 3
				&& passes[0].layer == 0 && passes[0].style == LayerRenderStyle::Solid
				&& passes[1].layer == 1 && passes[1].style == LayerRenderStyle::Aperture
				&& passes[2].layer == 1 && passes[2].style == LayerRenderStyle::Wireframe,
			"The render passes are not the selected Layer, the Layer behind drawn solid through "
			"its apertures, and one overlay");
	}

	// Turning the overlay off takes the outline away only: the Layer behind is
	// still drawn solid through its apertures.
	{
		auto const passes = renderPasses(0, 3, false);
		require(passes.size() == 2 && passes[1].style == LayerRenderStyle::Aperture,
			"Disabling the wireframe overlay removed more than the overlay pass");
	}

	{
		auto const passes = renderPasses(2, 3, true);
		require(passes.size() == 1 && passes[0].style == LayerRenderStyle::Solid,
			"The back-most Layer has no Layer behind it but produced more than its own pass");
	}

	// The same policy against a live Building: whatever the Layer count, exactly the
	// selected Layer and the Layer directly behind it are drawn.
	core::Building building("Render layer policy", 4, 2);
	building.addCorridor(0, 0, 4);
	building.addLayer();
	building.addLayer();
	require(building.getLayerCount() == 4, "A four-layer Building could not be created");

	for (uint32_t view = 0; view < building.getLayerCount(); ++view)
	{
		uint32_t drawn{ 0 };
		for (uint32_t layer = 0; layer < building.getLayerCount(); ++layer)
		{
			if (!isLayerDrawn(layer, view, building.getLayerCount())) continue;
			++drawn;
			require(layer == view || layer == view + 1,
				"A drawn Layer is neither the selected Layer nor the Layer directly behind it");
		}
		auto const expected = view + 1 < building.getLayerCount() ? 2u : 1u;
		require(drawn == expected,
			"The number of drawn Layers does not match the selected Layer");
	}
}

// Graph construction walks every adjacent Layer pair of the Building - 0<->1, 1<->2,
// and so on - rather than one hard-coded Fore/Back pass.  Every Layer therefore
// contributes Vertices, and each pair keeps its own inter-layer lookup so that a
// threshold is only ever joined to the pair it was authored on.
void graphConstructionWalksEveryAdjacentLayerPair()
{
	char const* roomNames[] = { "Corridor", "Basement", "Deep Cellar", "Catacomb" };

	for (uint32_t layerCount = 2; layerCount <= 5; ++layerCount)
	{
		core::Building building("Adjacent Pairs", 8, 2);
		while (building.getLayerCount() < layerCount) building.addLayer();

		std::vector<uint32_t> rooms;
		for (uint32_t layer = 0; layer < layerCount; ++layer)
		{
			rooms.push_back(building.addRoom(roomNames[layer], layer, 0, 0, 8, 1));
		}

		// One Marker per Layer gives every Layer a Vertex of its own, so the scan
		// coverage of each Layer can be observed directly.
		for (uint32_t layer = 0; layer < layerCount; ++layer)
		{
			building.addSectorMarker(rooms[layer], 0, 3.0f);
		}

		building.addSectorDoor(0, 0, 5);
		building.finishBuild();

		core::Graph graph(&building);
		graph.build();

		std::vector<uint32_t> verticesPerLayer(layerCount, 0);
		for (auto const& vertex : graph.getVertices())
		{
			auto const layer = vertex->getSector()->getLayerIndex();
			require(layer < layerCount, "A Vertex belongs to a Layer the Building does not have");
			++verticesPerLayer[layer];
		}

		for (uint32_t layer = 0; layer < layerCount; ++layer)
		{
			require(verticesPerLayer[layer] > 0,
				"A Layer deeper than the front pair contributed no Vertices to the Graph");
		}

		// The Door authored on the front pair joins that pair alone.
		uint32_t doorEdges{ 0 };
		for (auto const& edge : graph.getEdges())
		{
			if (edge->getType() != core::EdgeType::Door) continue;

			++doorEdges;

			auto const front = edge->getVertex(0)->getSector()->getLayerIndex();
			auto const back = edge->getVertex(1)->getSector()->getLayerIndex();

			require((front == 0 && back == 1) || (front == 1 && back == 0),
				"A Door Edge joined Layers outside the front adjacent pair");
		}

		require(doorEdges == 1, "The authored Door did not produce exactly one Edge");
	}
}

// Every threshold and Transit type pairs its inter-layer Vertices against the
// adjacent Layer pair it was authored on, not just the front pair.  A four-Layer
// Building carries one of each across three different pairs; the Graph must join
// every one of them to the Layer directly in front, and never skip a Layer.
void thresholdsAndTransitsPairTheirOwnAdjacentLayerPair()
{
	core::Building building("Deep Pairing", 40, 2);
	while (building.getLayerCount() < 4) building.addLayer();

	// Layer 0 - front-most.  Two stacked Corridors give the Layer 1 Ladder two
	// distinct landing Locations.
	building.addCorridor(0, 0, 0, 4, 1);
	building.addCorridor(0, 1, 0, 4, 1);

	// Layer 1 - back of pair 0<->1, landing Layer for the Layer 2 Transits, and
	// front of pair 1<->2.
	building.addRoom("Store", 1, 0, 0, 2, 1);
	building.addCorridor(1, 0, 8, 8, 1);
	building.addCorridor(1, 1, 8, 8, 1);
	building.addCorridor(1, 1, 30, 8, 1);   // Shuttle landing run

	// Layer 2 - back of pair 1<->2, landing Layer for the Layer 3 Transits, and
	// front of pair 2<->3.
	building.addRoom("Deep Store", 2, 0, 8, 2, 1);
	building.addRoom("Annexe", 2, 0, 11, 1, 1);
	building.addCorridor(2, 0, 16, 8, 1);
	building.addCorridor(2, 1, 16, 8, 1);
	building.addCorridor(2, 0, 24, 2, 1);
	building.addCorridor(2, 1, 24, 2, 1);
	building.addRoom("Stair Hall Lower", 2, 0, 28, 2, 1);
	building.addRoom("Bulkhead Left", 2, 0, 30, 2, 1);
	building.addRoom("Bulkhead Right", 2, 0, 32, 2, 1);
	building.addRoom("Stair Hall Upper", 2, 1, 28, 2, 1);

	// Layer 3 - back-most.
	building.addRoom("Deep Room", 3, 0, 16, 2, 1);
	building.addRoom("Deep Annexe", 3, 0, 19, 1, 1);

	// One of every threshold and Transit, each on a different adjacent Layer pair.
	// Pair 0<->1.
	building.addSectorDoor(0, 0, 1);
	building.addLadder(1, 0, 2, { 2, false, false });
	// Pair 1<->2.
	building.addSectorDoor(1, 0, 9);
	building.addSectorWindow(1, 0, 11, 1, 1, { true });
	building.addLadder(2, 0, 10, { 2, false, false });
	building.addLift(2, 0, 12, 1, 2);
	building.addShuttle(2, 1, 30, 8, { 1, 3, { 0, 5 }, 0 });
	// Pair 2<->3.
	building.addSectorDoor(2, 0, 17);
	building.addSectorWindow(2, 0, 19, 1, 1, { true });
	building.addLadder(3, 0, 18, { 2, false, false });
	building.addStairwell(3, 0, 28, 2, CORE_SIDE_LEFT);
	building.addStaircase(3, 0, 24, 2, CORE_SIDE_RIGHT);
	// A Bulkhead Door joins two Locations on its own Layer, so it pairs nothing.
	building.addSectorBulkheadDoor(2, 0, 32, CORE_SIDE_LEFT);

	building.finishBuild();

	core::Graph graph(&building);
	graph.build();

	// Nothing may reach across a Layer it did not pair with.
	for (auto const& edge : graph.getEdges())
	{
		auto const a = edge->getVertex(0)->getSector()->getLayerIndex();
		auto const b = edge->getVertex(1)->getSector()->getLayerIndex();
		require(a == b || a + 1 == b || b + 1 == a,
			"An Edge joined Layers that are not adjacent");
	}

	// Every Layer contributes Vertices.
	std::vector<uint32_t> verticesPerLayer(building.getLayerCount(), 0);
	for (auto const& vertex : graph.getVertices())
		++verticesPerLayer[vertex->getSector()->getLayerIndex()];
	for (uint32_t layer = 0; layer < building.getLayerCount(); ++layer)
		require(verticesPerLayer[layer] > 0, "A Layer contributed no Vertices to the Graph");

	auto countEdgesAcross = [&](core::EdgeType type, uint32_t front, uint32_t back)
	{
		uint32_t count{ 0 };
		for (auto const& edge : graph.getEdges())
		{
			if (edge->getType() != type) continue;
			auto const a = edge->getVertex(0)->getSector()->getLayerIndex();
			auto const b = edge->getVertex(1)->getSector()->getLayerIndex();
			if ((a == front && b == back) || (a == back && b == front)) ++count;
		}
		return count;
	};

	// Each authored threshold produced exactly one Edge across its own pair, and no
	// threshold paired a pair it was never authored on.
	// Pair 0<->1 holds only the one explicitly authored Door.  Pair 1<->2 holds the
	// explicit Door, the two landing Doors the Lift creates for its stops, and the two
	// Doors the Shuttle creates for its stops.  Pair 2<->3 again holds only the
	// explicit Door.  No threshold reaches any other pair.
	require(countEdgesAcross(core::EdgeType::Door, 0, 1) == 1,
		"Pair 0<->1 should hold exactly the one authored Door");
	require(countEdgesAcross(core::EdgeType::Door, 1, 2) == 5,
		"Pair 1<->2 should hold the authored Door plus the Lift and Shuttle landing Doors");
	require(countEdgesAcross(core::EdgeType::Door, 2, 3) == 1,
		"Pair 2<->3 should hold exactly the one authored Door");
	require(countEdgesAcross(core::EdgeType::Window, 1, 2) == 1,
		"The Window authored on pair 1<->2 did not pair there");
	require(countEdgesAcross(core::EdgeType::Window, 2, 3) == 1,
		"The Window authored on pair 2<->3 did not pair there");
	require(countEdgesAcross(core::EdgeType::Window, 0, 1) == 0,
		"A Window paired a Layer pair it was never authored on");
	require(countEdgesAcross(core::EdgeType::Door, 0, 2) == 0
		&& countEdgesAcross(core::EdgeType::Door, 1, 3) == 0,
		"A Door skipped a Layer");

	uint32_t bulkheadEdges{ 0 };
	for (auto const& edge : graph.getEdges())
	{
		if (edge->getType() != core::EdgeType::BulkheadDoor) continue;
		require(edge->getVertex(0)->getSector()->getLayerIndex()
			== edge->getVertex(1)->getSector()->getLayerIndex(),
			"A Bulkhead Door Edge crossed Layers");
		++bulkheadEdges;
	}
	require(bulkheadEdges == 1, "The Bulkhead Door did not produce one same-Layer Edge");

	// Every Transit sits on the Layer it was authored on, and mounts onto the Layer
	// directly in front of it - never any other.
	struct TransitExpectation
	{
		core::SectorType sectorType;
		core::EdgeType mountType;
		uint32_t layer;
	};

	// Enclosed Lifts and Shuttles reach their landing Layer through the landing
	// Doors authored in front of them rather than through mount edges, so they are
	// covered by the Door counts above instead of by a mount expectation here.
	std::vector<TransitExpectation> const expected{
		{ core::SectorType::Ladder, core::EdgeType::LadderMount, 1 },
		{ core::SectorType::Ladder, core::EdgeType::LadderMount, 2 },
		{ core::SectorType::Ladder, core::EdgeType::LadderMount, 3 },
		{ core::SectorType::Stairwell, core::EdgeType::StairwellMount, 3 },
		{ core::SectorType::Staircase, core::EdgeType::StaircaseMount, 3 },
	};

	for (auto const& want : expected)
	{
		uint32_t mounts{ 0 };
		uint32_t strays{ 0 };
		for (auto const& edge : graph.getEdges())
		{
			if (edge->getType() != want.mountType) continue;
			auto const a = edge->getVertex(0)->getSector();
			auto const b = edge->getVertex(1)->getSector();
			auto const transit = a->getType() == want.sectorType ? a : b;
			auto const other = transit == a ? b : a;
			if (transit->getType() != want.sectorType) continue;
			// Only this expectation's own Transit; other Layers are checked separately.
			if (transit->getLayerIndex() != want.layer) continue;
			if (other->getLayerIndex() + 1 == transit->getLayerIndex()) ++mounts;
			else ++strays;
		}
		require(mounts > 0, "A Transit never mounted onto the Layer directly in front of it");
		require(strays == 0, "A Transit mounted onto a Layer other than the one in front of it");
	}

	// The authored pairings survive a save and reload unchanged.
	core::SerializationWorkData workData;
	auto writer = core::YamlSerializer::toString();
	building.serialize(*writer, workData);
	writer->serialize();
	auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
	reader->deserialize();
	core::Building reloaded("placeholder", 1, 1);
	require(reloaded.deserialize(*reader, workData), "A deep Building did not round-trip");
	require(reloaded.getLayerCount() == building.getLayerCount(),
		"Round-tripping changed the Layer count");

	core::Graph reloadedGraph(&reloaded);
	reloadedGraph.build();
	uint32_t deepDoors{ 0 };
	for (auto const& edge : reloadedGraph.getEdges())
	{
		if (edge->getType() != core::EdgeType::Door) continue;
		auto const a = edge->getVertex(0)->getSector()->getLayerIndex();
		auto const b = edge->getVertex(1)->getSector()->getLayerIndex();
		if ((a == 2 && b == 3) || (a == 3 && b == 2)) ++deepDoors;
	}
	require(deepDoors == 1, "The reloaded Building lost its deep Door pairing");
}

void doorAndWindowRemovalWorksOnDeepLayerPairs()
{
	// Regression for ticket #19: removeSectorDoor/Window looped over absolute
	// Layer indices and passed them to Door/Window::getSector(), which expects a
	// pair side (0 or 1). With three Layers, getSector(2) asserted or read past
	// the end of mSectors.
	core::Building building("Deep pair removal", 8, 3);
	building.addLayer();
	building.addCorridor(0, 0, 8);
	building.addRoom("Basement", 1, 0, 0, 8, 1);
	building.addRoom("Cellar", 2, 0, 0, 8, 1);

	auto const door = building.addSectorDoor(0, 0, 3);
	auto const window = building.addSectorWindow(1, 0, 5, 1, 1, { true });

	building.pauseSimulation();
	require(building.removeSectorDoor(door.door.sector->getIndex(), door.door.index),
		"Door on a three-layer Building could not be removed");
	require(building.removeSectorWindow(window.window.sector->getIndex(), window.window.index),
		"Window on a three-layer Building could not be removed");
}

void shuttleDoorCandidatesAreFoundOnTheShuttleLayer()
{
	// Regression for ticket #22: the editor passed the Layer a Door is authored
	// on to getShuttleStopCandidatesForDoor, which matches the Shuttle Transit's
	// own Layer - one behind.  Dropping a Door onto a Shuttle stop column found no
	// candidate, so the Add Shuttle stop popup never appeared.
	auto const probe = [](uint32_t shuttleLayer)
	{
		core::Building building("Shuttle door candidate layers", 32, 2);
		while (building.getLayerCount() <= shuttleLayer) building.addLayer();
		for (uint32_t layer = 0; layer < shuttleLayer; ++layer)
			building.addCorridor(layer, 0, 0, 31, 1);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.doorMask = 0b101;
		auto const created = building.addShuttle(shuttleLayer, 0, 0, 27, options);
		building.finishBuild();
		auto const shuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(
			created.shuttle.sector);
		require(shuttle && shuttle->getLayerIndex() == shuttleLayer,
			"The Shuttle was not authored on the probed Layer");

		// A new stop at offset 9 lines carriage 0's first door up with column 9 of
		// the landing Layer directly in front of the Shuttle.
		auto const found = building.getShuttleStopCandidatesForDoor(shuttleLayer, 0, 9);
		require(any_of(found.begin(), found.end(), [&](auto const& candidate)
			{
				return candidate.sectorIndex == shuttle->getIndex() && candidate.stopOffset == 9;
			}),
			"No Shuttle stop candidate was offered for a Door over a Shuttle door");
		// The Layer the Door is authored on holds no Shuttle, so querying it as a
		// Shuttle Layer must find nothing.
		require(building.getShuttleStopCandidatesForDoor(shuttleLayer - 1, 0, 9).empty(),
			"A Door Layer was treated as a Shuttle Layer");
	};
	probe(1);
	probe(2);
}

void candidateReplayIncludesAllLayers()
{
	// Regression for ticket #17: validation candidates were constructed with the
	// default two Layers, so any construction record on Layer >= 2 threw an out-of-
	// bounds error and every rebuild-based edit was reported as invalid.
	core::Building building("Deep candidate replay", 8, 3);
	building.addLayer();
	auto const fore = building.addCorridor(0, 0, 8);
	building.addRoom("Deep room", 2, 0, 0, 8, 1);
	building.finishBuild();

	auto resize = building.planResizeLocation(fore, 0, 0, 4, 1);
	require(resize.valid,
		("Layer-0 Location edit was rejected on a three-layer Building: " + resize.diagnostic).c_str());

	building.pauseSimulation();
	auto const resized = building.applyLocationEdit(resize);
	require(building.getSector(resized) && building.getSector(resized)->getCellsWide() == 4,
		"Layer-0 Location edit was not applied on a three-layer Building");
}

// Regression for ticket #18: the Transit edit planners read blockers from Layer 1
// and landings from Layer 0 whatever Layer the Transit was authored on.  A Transit
// on Layer 2 or deeper therefore always failed to plan - its own landing Locations
// were misread as blockers - and the apply functions handed back the Sector found
// on Layer 1 instead of the edited Transit.  Each Transit type is probed on Layer 1,
// where the old constants happened to be right, and on Layer 2, where they were not.
void liftEditsUseTheLiftsOwnLayer()
{
	auto const probe = [](uint32_t transitLayer)
	{
		core::Building building("Deep Lift edit", 12, 3);
		while (building.getLayerCount() <= transitLayer) building.addLayer();
		building.addRoom("Landing A", transitLayer - 1, 0, 0, 12, 1);
		building.addRoom("Landing B", transitLayer - 1, 1, 0, 12, 1);
		building.addRoom("Landing C", transitLayer - 1, 2, 0, 12, 1);
		// A neighbour on the Lift's own Layer, to prove the blocker scan still runs
		// against that Layer rather than being skipped.
		building.addRoom("Shaft neighbour", transitLayer, 1, 8, 1, 1);
		auto const created = building.addLift(transitLayer, 0, 2, 1, 3);
		building.finishBuild();
		building.pauseSimulation();
		auto const index = created.lift.sector->getIndex();
		require(created.lift.sector->getLayerIndex() == transitLayer,
			"The Lift was not authored on the probed Layer");

		auto const blocked = building.planResizeLift(index, 8, 0, 1, 3);
		require(!blocked.valid
			&& blocked.diagnostic.find("8,1 blocks the Lift") != std::string::npos,
			("A Lift move into a Sector on its own Layer was not blocked: "
				+ blocked.diagnostic).c_str());

		auto const plan = building.planResizeLift(index, 5, 0, 1, 3);
		require(plan.valid && plan.move,
			("A Lift move on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + plan.diagnostic).c_str());
		require(plan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2 },
			"The Lift stops were not derived from the Layer in front of the Lift");
		auto const moved = building.applyLiftEdit(plan);
		require(moved == index,
			"applyLiftEdit returned a Sector from the wrong Layer");
		auto const lift = std::dynamic_pointer_cast<const core::LiftTransit>(
			building.getSector(moved));
		require(lift && lift->getCellX() == 5 && lift->getLayerIndex() == transitLayer
			&& lift->getNumStops() == 3,
			"The Lift was not moved on its own Layer");

		auto const stopRemoval = building.planRemoveLiftStop(moved, 1);
		require(stopRemoval.valid,
			("Deleting a Lift stop on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + stopRemoval.diagnostic).c_str());
		auto const trimmed = building.applyLiftEdit(stopRemoval);
		auto const trimmedLift = std::dynamic_pointer_cast<const core::LiftTransit>(
			building.getSector(trimmed));
		require(trimmedLift && trimmedLift->getNumStops() == 2
			&& trimmedLift->getLayerIndex() == transitLayer,
			"The deep Lift did not lose its deleted stop");

		auto const removal = building.planRemoveLift(trimmed);
		require(removal.valid,
			("A Lift deletion on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + removal.diagnostic).c_str());
		require(building.applyLiftEdit(removal) == ~0u,
			"Deleting a deep Lift did not return the removed-sector sentinel");
	};
	probe(1);
	probe(2);
}

void shuttleEditsUseTheShuttlesOwnLayer()
{
	auto const probe = [](uint32_t transitLayer)
	{
		core::Building building("Deep Shuttle edit", 32, 2);
		while (building.getLayerCount() <= transitLayer) building.addLayer();
		building.addCorridor(transitLayer - 1, 0, 0, 31, 1);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101;
		// A neighbour on the Shuttle's own Layer keeps the blocker scan honest.
		building.addRoom("Track neighbour", transitLayer, 0, 29, 1, 1);
		auto const created = building.addShuttle(transitLayer, 0, 0, 27, options);
		building.finishBuild();
		building.pauseSimulation();
		auto const index = created.shuttle.sector->getIndex();
		require(created.shuttle.sector->getLayerIndex() == transitLayer,
			"The Shuttle was not authored on the probed Layer");

		auto const plan = building.planResizeShuttle(index, 2, 0, 27);
		require(plan.valid && plan.move,
			("A Shuttle move on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + plan.diagnostic).c_str());
		auto const moved = building.applyShuttleEdit(plan);
		require(moved == index,
			"applyShuttleEdit returned a Sector from the wrong Layer");
		auto const shuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(
			building.getSector(moved));
		require(shuttle && shuttle->getCellX() == 2 && shuttle->getLayerIndex() == transitLayer
			&& shuttle->getNumStops() == 2,
			"The Shuttle was not moved on its own Layer");

		auto const blocked = building.planResizeShuttle(moved, 3, 0, 27);
		require(!blocked.valid
			&& blocked.diagnostic.find("29,0 blocks the Shuttle") != std::string::npos,
			("A Shuttle track grown into a Sector on its own Layer was not blocked: "
				+ blocked.diagnostic).c_str());
		auto const addStop = building.planAddShuttleStop(moved, 9);
		require(addStop.valid,
			("Adding a Shuttle stop on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + addStop.diagnostic).c_str());
		auto const widened = building.applyShuttleEdit(addStop);
		auto const widenedShuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(
			building.getSector(widened));
		require(widenedShuttle && widenedShuttle->getNumStops() == 3
			&& widenedShuttle->getLayerIndex() == transitLayer,
			"The deep Shuttle did not gain its new stop");
		auto const removeStop = building.planRemoveShuttleStop(widened, 1);
		require(removeStop.valid,
			("Deleting a Shuttle stop on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + removeStop.diagnostic).c_str());
		auto const narrowed = building.applyShuttleEdit(removeStop);
		auto const narrowedShuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(
			building.getSector(narrowed));
		require(narrowedShuttle && narrowedShuttle->getNumStops() == 2,
			"The deep Shuttle did not lose its deleted stop");

		auto const removal = building.planRemoveShuttle(narrowed);
		require(removal.valid,
			("A Shuttle deletion on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + removal.diagnostic).c_str());
		require(building.applyShuttleEdit(removal) == ~0u,
			"Deleting a deep Shuttle did not return the removed-sector sentinel");
	};
	probe(1);
	probe(2);
}

void shuttleDeletionRemovesWindowsOverTheShuttleItself()
{
	// A Window looks into the Layer directly behind the Layer it is authored on, so
	// a Window resting on a Shuttle depends on that Shuttle's cells on the Shuttle's
	// own Layer.  Deleting the Shuttle must take the dependent Window with it.
	auto const probe = [](uint32_t transitLayer)
	{
		core::Building building("Deep Shuttle Window dependency", 32, 2);
		while (building.getLayerCount() <= transitLayer) building.addLayer();
		building.addCorridor(transitLayer - 1, 0, 0, 31, 1);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.doorMask = 0b101;
		auto const created = building.addShuttle(transitLayer, 0, 0, 27, options);
		// Columns 8 and 9 carry no carriage door, so the Window lands on bare Shuttle.
		building.addSectorWindow(transitLayer - 1, 0, 8, 2, 1);
		building.finishBuild();
		building.pauseSimulation();
		auto const shuttleIndex = created.shuttle.sector->getIndex();
		auto const shuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(
			building.getSector(shuttleIndex));
		require(shuttle && shuttle->getLayerIndex() == transitLayer,
			"The Shuttle was not authored on the probed Layer");

		auto const plan = building.planRemoveShuttle(shuttleIndex);
		require(plan.valid,
			("Deleting a Shuttle with a dependent Window on Layer "
				+ std::to_string(transitLayer) + " was rejected: " + plan.diagnostic).c_str());
		require(any_of(plan.consequences.begin(), plan.consequences.end(),
			[](std::string const& consequence)
			{
				return consequence.find("dependent Window") != std::string::npos;
			}),
			"The dependent Window was not reported as a consequence of Shuttle deletion");
		require(building.applyShuttleEdit(plan) == ~0u,
			"Deleting a deep Shuttle with a dependent Window failed");
		require(!static_cast<core::Building const&>(building).getLayer(transitLayer)
			->getCellDefinition(8, 0).occupied(),
			"The deleted Shuttle still occupies its own Layer");
	};
	probe(1);
	probe(2);
}

void ladderEditsUseTheLaddersOwnLayer()
{
	auto const probe = [](uint32_t transitLayer)
	{
		core::Building building("Deep Ladder edit", 10, 5);
		while (building.getLayerCount() <= transitLayer) building.addLayer();
		for (uint32_t y = 0; y < 5; ++y) building.addCorridor(transitLayer - 1, y, 0, 10, 1);
		auto const created = building.addLadder(transitLayer, 0, 1, { 3, false, true });
		building.finishBuild();
		building.pauseSimulation();
		auto const index = created.ladder.sector->getIndex();
		require(created.ladder.sector->getLayerIndex() == transitLayer,
			"The Ladder was not authored on the probed Layer");

		core::Building::CreateLadderOptions edited{ 3, true, false, 3 };
		auto const plan = building.planResizeLadder(index, 4, 1, edited);
		require(plan.valid && plan.move,
			("A Ladder move on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + plan.diagnostic).c_str());
		auto const moved = building.applyLadderEdit(plan);
		require(moved == index,
			"applyLadderEdit returned a Sector from the wrong Layer");
		auto const ladder = std::dynamic_pointer_cast<const core::LadderTransit>(
			building.getSector(moved));
		require(ladder && ladder->getCellX() == 4 && ladder->getLayerIndex() == transitLayer
			&& ladder->getDecksHigh() == 3,
			"The Ladder was not moved on its own Layer");

		auto const removal = building.planRemoveLadder(moved);
		require(removal.valid,
			("A Ladder deletion on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + removal.diagnostic).c_str());
		require(building.applyLadderEdit(removal) == ~0u,
			"Deleting a deep Ladder did not return the removed-sector sentinel");
	};
	probe(1);
	probe(2);
}

void stairwellEditsUseTheStairwellsOwnLayer()
{
	auto const probe = [](uint32_t transitLayer)
	{
		core::Building building("Deep Stairwell edit", 10, 5);
		while (building.getLayerCount() <= transitLayer) building.addLayer();
		for (uint32_t y = 0; y < 5; ++y) building.addCorridor(transitLayer - 1, y, 0, 10, 1);
		auto const created = building.addStairwell(transitLayer, 0, 1,
			core::Building::CreateStairwellOptions{ 3, CORE_SIDE_LEFT });
		building.finishBuild();
		building.pauseSimulation();
		auto const index = created.sectorIndex;
		require(building.getSector(index)->getLayerIndex() == transitLayer,
			"The Stairwell was not authored on the probed Layer");

		core::Building::CreateStairwellOptions edited{ 3, CORE_SIDE_RIGHT, 2, 3 };
		auto const plan = building.planResizeStairwell(index, 4, 1, edited);
		require(plan.valid && plan.move,
			("A Stairwell move on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + plan.diagnostic).c_str());
		auto const moved = building.applyStairwellEdit(plan);
		require(moved == index,
			"applyStairwellEdit returned a Sector from the wrong Layer");
		auto const stairwell = std::dynamic_pointer_cast<const core::StairwellTransit>(
			building.getSector(moved));
		require(stairwell && stairwell->getCellX() == 4
			&& stairwell->getLayerIndex() == transitLayer && stairwell->getDecksHigh() == 3,
			"The Stairwell was not moved on its own Layer");

		auto const removal = building.planRemoveStairwell(moved);
		require(removal.valid,
			("A Stairwell deletion on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + removal.diagnostic).c_str());
		require(building.applyStairwellEdit(removal) == ~0u,
			"Deleting a deep Stairwell did not return the removed-sector sentinel");
	};
	probe(1);
	probe(2);
}

void staircaseEditsReturnTheStaircaseOwnLayer()
{
	// planResizeStaircase already derives its Layers from the Staircase itself, but
	// applyStaircaseEdit still read its return value from Layer 1.
	auto const probe = [](uint32_t transitLayer)
	{
		core::Building building("Deep Staircase edit", 6, 3);
		while (building.getLayerCount() <= transitLayer) building.addLayer();
		building.addCorridor(transitLayer - 1, 0, 0, 1, 1);
		building.addCorridor(transitLayer - 1, 0, 3, 1, 1);
		building.addCorridor(transitLayer - 1, 1, 0, 1, 1);
		building.addCorridor(transitLayer - 1, 1, 3, 1, 1);
		auto const index = building.addStaircase(transitLayer, 0, 0, 4, CORE_SIDE_RIGHT, 1.25f);
		building.finishBuild();
		building.pauseSimulation();
		require(building.getSector(index)->getLayerIndex() == transitLayer,
			"The Staircase was not authored on the probed Layer");

		auto const plan = building.planResizeStaircase(index, 0, 0,
			{ 4, CORE_SIDE_LEFT, -0.75f });
		require(plan.valid,
			("A Staircase flip on Layer " + std::to_string(transitLayer)
				+ " was rejected: " + plan.diagnostic).c_str());
		auto const flipped = building.applyStaircaseEdit(plan);
		require(flipped == index,
			"applyStaircaseEdit returned a Sector from the wrong Layer");
		auto const transit = std::dynamic_pointer_cast<const core::StaircaseTransit>(
			building.getSector(flipped));
		require(transit && transit->getLayerIndex() == transitLayer
			&& transit->getRiseSide() == CORE_SIDE_LEFT,
			"The Staircase was not edited on its own Layer");
	};
	probe(1);
	probe(2);
}

void runSerializationSmokeChecks()
{
	layerHelperApiIsConsistentWithLayerCount();
	onlyTheSelectedLayerAndTheLayerBehindAreDrawn();
	graphConstructionWalksEveryAdjacentLayerPair();
	thresholdsAndTransitsPairTheirOwnAdjacentLayerPair();
	transitsOnTheLayerBehindAreOnlyDrawnThroughApertures();
	stringYamlRoundTripsPrimitiveValues();
	fileYamlRoundTrips();
	lateWriteFailurePreservesThePreviousSaveFile();
	failedSavePreservesUnsavedChangesState();
	malformedValuesAndInvalidUsageThrowUsefulErrors();
	buildingRoundTripsAuthoredStateAndAgents();
	platformLiftStopDurationRoundTrips();
	legacyBuildingYamlStillLoads();
	legacyVersion3BuildingYamlStillLoadsWithDefaultLayers();
	version4BuildingYamlStillLoads();
	layerFieldsAcceptLegacyNamesAndIndices();
	buildingLayerNamesRoundTrip();
	addedLayersAppendToTheBackAndRoundTrip();
	layerCountIsCappedAtCoreMaxLayers();
	deletingAMiddleLayerCompactsTheLayersAboveIt();
	deletingTheFrontLayerRemovesTransitsOneLayerBehind();
	layerDeletionPreservesAuthoredRecordDependencies();
	layerDeletionKeepsAtLeastTwoLayers();
	locationEditsArePlannedAndAppliedAtomically();
	editedShuttleRoundTripsWithoutSchemaChanges();
	enclosedLiftsSupportMultiDeckRooms();
	stopDerivingAddLiftRejectsInvalidLayerIndex();
	stairwellSectorsAreCanvasSelectable();
	staircasesConnectAdjacentCorridorsAndRoundTrip();
	laddersCanBeValidatedEditedAndDeleted();
	stairwellsCanBeValidatedEditedAndDeleted();
	physicalControlsPreferDistinctWallPositions();
	bulkheadDoorsSupportIndependentObjectEditing();
	recentFilesPersistAcrossStartup();
	serializableTracksModificationState();
	doorAndWindowRemovalWorksOnDeepLayerPairs();
	candidateReplayIncludesAllLayers();
	shuttleDoorCandidatesAreFoundOnTheShuttleLayer();
	liftEditsUseTheLiftsOwnLayer();
	shuttleEditsUseTheShuttlesOwnLayer();
	shuttleDeletionRemovesWindowsOverTheShuttleItself();
	ladderEditsUseTheLaddersOwnLayer();
	stairwellEditsUseTheStairwellsOwnLayer();
	staircaseEditsReturnTheStaircaseOwnLayer();
}
