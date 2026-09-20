#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
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

#if !defined(_WIN32)
	// #92: a save addressed at a symbolic link must update the link's target and
	// leave the link itself in place, as the pre-#62 std::ofstream save did.
	void saveThroughSymlinkUpdatesItsTarget()
	{
		namespace filesystem = std::filesystem;
		filesystem::path const directory = filesystem::temp_directory_path() / "pf-save-symlink-smoke";
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
		filesystem::path const target = directory / "target.yaml";
		filesystem::path const link = directory / "save.yaml";
		{
			std::ofstream original(target, std::ios::binary);
			original << "old-target\n";
		}
		// A relative link, as a user would typically create one.
		filesystem::create_symlink(filesystem::path("target.yaml"), link);

		auto writer = core::YamlSerializer::toFile(link.string());
		writer->beginMap("");
		writer->writeString("payload", std::string("via symlink"));
		writer->endMap();
		writer->serialize();

		require(filesystem::is_symlink(filesystem::symlink_status(link)),
			"save through a symlink destroyed the symlink");
		require(filesystem::read_symlink(link) == filesystem::path("target.yaml"),
			"save through a symlink repointed the symlink");
		std::ifstream in(target, std::ios::binary);
		std::string const contents((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
		require(contents.find("via symlink") != std::string::npos,
			"save through a symlink did not update its target");
		bool tempFileLeftBehind = false;
		for (auto const& entry : filesystem::directory_iterator(directory))
		{
			if (entry.path().filename().string().find(".saving.tmp") != std::string::npos)
				tempFileLeftBehind = true;
		}
		require(!tempFileLeftBehind, "save through a symlink left a temporary file behind");
	}

	// #92: replacing an existing save file must keep its permission mode,
	// including a restrictive 0600, instead of resetting it to the umask mode.
	void savePreservesExistingFilePermissions()
	{
		namespace filesystem = std::filesystem;
		filesystem::path const directory = filesystem::temp_directory_path() / "pf-save-permissions-smoke";
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
		{
			std::ofstream original(destination, std::ios::binary);
			original << "old\n";
		}
		auto const privateMode = filesystem::perms::owner_read | filesystem::perms::owner_write;
		filesystem::permissions(destination, privateMode, filesystem::perm_options::replace);

		auto writer = core::YamlSerializer::toFile(destination.string());
		writer->beginMap("");
		writer->writeString("payload", std::string("permissions"));
		writer->endMap();
		writer->serialize();

		require(filesystem::status(destination).permissions() == privateMode,
			"save reset a restrictive permission mode to the umask mode");
		auto reader = core::YamlSerializer::fromFile(destination.string());
		reader->deserialize();
		require(reader->readString("payload") == "permissions",
			"permission-preserving save did not install the new contents");
	}
#endif

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
		require(yaml.find("version: 8") != std::string::npos
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

	// Ticket #60: a hand-edited Agent position must refuse the open rather than
	// seed the world with an Agent that can be neither drawn nor hit-tested.
	void agentRestoreRejectsMalformedPositions()
	{
		core::Building original("Position probe", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		original.createAgent("Probe agent", 0, 0, 0.75f);

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		auto const agentsAt = yaml.find("\nagents:");
		require(yaml.find("localX: 0.75") != std::string::npos && agentsAt != std::string::npos,
			"Agent position did not serialize as expected");
		auto const head = yaml.substr(0, agentsAt + 1);

		auto rejects = [&](std::string const& agentsBlock, std::string const& what)
		{
			// A reused instance: the previous contents must not survive the failed
			// open, and neither must the Agent the bad record half-restored.
			core::Building reused("reused", 2, 2);
			{
				auto goodReader = core::YamlSerializer::fromString(yaml);
				goodReader->deserialize();
				require(reused.deserialize(*goodReader, workData),
					"The good Building did not load into a reused instance");
				require(reused.getSimulationSnapshot().agents.size() == 1,
					"The reused instance did not start with one Agent");
			}

			auto reader = core::YamlSerializer::fromString(head + agentsBlock);
			reader->deserialize();
			bool threw{ false };
			std::string message;
			try
			{
				reused.deserialize(*reader, workData);
			}
			catch (core::SerializationException const& error)
			{
				threw = true;
				message = error.what();
			}
			require(threw, ("Malformed " + what + " was accepted").c_str());
			require(message.find("position") != std::string::npos
				|| message.find("path") != std::string::npos,
				("Malformed " + what + " gave an imprecise diagnostic: " + message).c_str());
			require(reused.getSimulationSnapshot().agents.empty(),
				("Malformed " + what + " left an Agent owned by the reused Building").c_str());
			require(reused.getSector(0)->getAgents().empty(),
				("Malformed " + what + " left an Agent in the Sector").c_str());
		};

		auto positioned = [](std::string const& localX, std::string const& localY)
		{
			return "agents:\n"
				"  - id: 1\n"
				"    agent:\n"
				"      name: Probe agent\n"
				"      flags: 0\n"
				"    sector: 0\n"
				"    localX: " + localX + "\n"
				"    localY: " + localY + "\n";
		};

		rejects(positioned(".nan", "0"), "NaN localX");
		rejects(positioned("0.75", ".nan"), "NaN localY");
		rejects(positioned(".inf", "0"), "infinite localX");
		rejects(positioned("0.75", "-.inf"), "negative infinite localY");
		// A finite point beyond the Sector's right edge.
		rejects(positioned("99.5", "0"), "finite out-of-Sector localX");
		// A finite point on the Room's upper deck, which has no Walkway there.
		rejects(positioned("1.5", "1"), "finite position on non-traversable floor");
	}

	void agentRestoreRejectsBackgroundAndUnreachableDestination()
	{
		// Two Door-joined pairs of Rooms on separate Layers, plus a Background.
		// A<->B and D<->E each connect, but nothing joins the pairs, so a route
		// from A to D cannot be rebuilt.
		core::Building original("Background and route probe", 16, 3);
		original.addLayer();
		original.addLayer();
		original.addRoom("A", 0, 0, 0, 3, 2);
		original.addRoom("B", 1, 0, 0, 3, 2);
		original.addRoom("D", 2, 0, 5, 3, 2);
		original.addRoom("E", 3, 0, 5, 3, 2);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = 1;
		original.addSectorDoor(0, 0, 2, doorOptions);
		original.addSectorDoor(2, 0, 7, doorOptions);
		original.addBackground(3, 0, 9, 7, 2);
		original.finishBuild();
		uint32_t backgroundIndex{ ~0u };
		for (uint32_t i = 0; i < original.getNumSectors(); ++i)
		{
			if (original.getSector(i)->getType() == core::SectorType::Background)
				backgroundIndex = i;
		}
		require(backgroundIndex != ~0u, "The probe Building has no Background to target");
		require(original.getGraph()->getVertices().size() >= 4,
			"The probe Building built no route vertices, so the route cases prove nothing");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		auto const agentsAt = yaml.find("\nagents:");
		require(agentsAt != std::string::npos, "The probe Building serialized no agents section");
		auto const head = yaml.substr(0, agentsAt + 1);

		auto rejects = [&](std::string const& agentsBlock, std::string const& what)
		{
			core::Building reused("reused", 2, 2);
			auto reader = core::YamlSerializer::fromString(head + agentsBlock);
			reader->deserialize();
			bool threw{ false };
			try
			{
				reused.deserialize(*reader, workData);
			}
			catch (core::SerializationException const&)
			{
				threw = true;
			}
			require(threw, ("Malformed " + what + " was accepted").c_str());
			require(reused.getSimulationSnapshot().agents.empty(),
				("Malformed " + what + " left an Agent owned by the reused Building").c_str());
			for (uint32_t i = 0; i < reused.getNumSectors(); ++i)
			{
				require(reused.getSector(i)->getAgents().empty(),
					("Malformed " + what + " left an Agent in a Sector").c_str());
			}
		};

		rejects("agents:\n"
			"  - id: 1\n"
			"    agent:\n"
			"      name: Nowhere agent\n"
			"      flags: 0\n"
			"    sector: " + std::to_string(backgroundIndex) + "\n"
			"    localX: 1.5\n"
			"    localY: 0\n", "Background position");

		// Sector 0 is Room A and sector 2 is Room D: both hold route vertices,
		// but no Door joins the pairs, so the saved route cannot be rebuilt.
		rejects("agents:\n"
			"  - id: 1\n"
			"    agent:\n"
			"      name: Stranded agent\n"
			"      flags: 0\n"
			"    sector: 0\n"
			"    localX: 1.5\n"
			"    localY: 0\n"
			"    path:\n"
			"      destinationSector: 2\n"
			"      destinationLocalX: 1.5\n"
			"      destinationLocalY: 0\n"
			"      active: false\n", "unreachable destination");
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

	// Ticket #81: OpenUp is the canonical authored Door opening style. It is carried
	// by creation options, persisted as openStyle: openUp, replayed on load, and
	// legacy records without a style default to OpenUp while unknown names fail clearly.
	void doorOpeningStyleIsAuthoredPersistedAndLegacyDefaulted()
	{
		auto findDoor = [](core::Building const& building, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = building.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};

		core::Building original("OpenUp door", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenUp;
		auto const created = original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		auto const createdDoor = findDoor(original, created.door.sector->getIndex());
		require(createdDoor && createdDoor->getOpenStyle() == core::Door::OpenStyle::OpenUp,
			"Ordinary Door creation did not carry the OpenUp opening style");
		core::Building::CreateDoorOptions readBack;
		require(original.getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenUp,
			"Authored Door options did not report the OpenUp opening style");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("openStyle: openUp") != std::string::npos,
			"Building YAML did not persist the Door's openStyle: openUp");

		core::Building loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "OpenUp Door building did not round-trip");
		auto const loadedDoor = findDoor(loaded, created.door.sector->getIndex());
		require(loadedDoor && loadedDoor->getOpenStyle() == core::Door::OpenStyle::OpenUp,
			"Loaded Door lost its OpenUp opening style");
		require(loaded.getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenUp,
			"Loaded Door options lost the OpenUp opening style");

		// A move replays the authored record at a new position; the style rides along.
		loaded.pauseSimulation();
		uint32_t loadedDoorObjectIndex{ ~0u };
		{
			auto const sector = loaded.getSector(created.door.sector->getIndex());
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const candidate = sector->getObject(i);
				if (candidate && candidate->getObjectType() == core::SectorObjectType::Door
					&& static_pointer_cast<const core::DoorSectorObject>(candidate)->getDoor() == loadedDoor)
					{ loadedDoorObjectIndex = i; break; }
			}
		}
		require(loadedDoorObjectIndex != ~0u, "Loaded Door object could not be found");
		auto const movePlan = loaded.planMoveSectorObject(
			loadedDoor->getFrontSector()->getIndex(), loadedDoorObjectIndex, 5, 0);
		require(movePlan.valid, "OpenUp Door move plan was rejected");
		require(loaded.applyObjectMove(movePlan) != nullptr, "OpenUp Door move failed");
		require(loaded.getSectorDoorOptions(0, 0, 5, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenUp,
			"Moving the Door did not preserve its OpenUp opening style");

		// Legacy records written before opening styles existed load as OpenUp.
		std::string legacyYaml;
		{
			std::string const needle = "    openStyle: openUp\n";
			auto const at = yaml.find(needle);
			require(at != std::string::npos, "Door openStyle line was not where expected");
			legacyYaml = yaml.substr(0, at) + yaml.substr(at + needle.size());
		}
		core::Building legacy("placeholder", 1, 1);
		{
			auto legacyReader = core::YamlSerializer::fromString(legacyYaml);
			legacyReader->deserialize();
			require(legacy.deserialize(*legacyReader, workData),
				"Legacy Door record without an openStyle no longer loads");
			auto const legacyDoor = findDoor(legacy, created.door.sector->getIndex());
			require(legacyDoor && legacyDoor->getOpenStyle() == core::Door::OpenStyle::OpenUp,
				"Legacy Door without a style did not load as OpenUp");
		}

		// An unknown persisted style name fails with a clear serialization error.
		{
			auto const at = legacyYaml.find("crossingLanes:");
			require(at != std::string::npos, "Door crossingLanes line was not where expected");
			auto const lineEnd = legacyYaml.find('\n', at);
			auto const unknownStyle = legacyYaml.substr(0, lineEnd + 1)
				+ "    openStyle: openSideways\n" + legacyYaml.substr(lineEnd + 1);
			core::Building rejected("placeholder", 1, 1);
			auto badReader = core::YamlSerializer::fromString(unknownStyle);
			badReader->deserialize();
			bool threw{ false };
			std::string message;
			try
			{
				rejected.deserialize(*badReader, workData);
			}
			catch (core::SerializationException const& error)
			{
				threw = true;
				message = error.what();
			}
			require(threw, "Unknown Door opening style was accepted");
			require(message.find("opening style") != std::string::npos
				&& message.find("openSideways") != std::string::npos,
				("Unknown Door opening style gave an imprecise diagnostic: " + message).c_str());
		}
	}

	// A regular Door's deck span is authored, persisted and replayed.  A Door
	// record written before the span existed replays one deck tall, which is all
	// any pre-span build could ever have shown.
	void doorDeckSpanPersistsAndLegacyRecordsStayOneDeckTall()
	{
		auto findDoor = [](core::Building const& building, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = building.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};

		core::Building original("Two deck door", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = 1;
		doorOptions.decksHigh = 2;
		auto const created = original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		auto const createdDoor = findDoor(original, created.door.sector->getIndex());
		require(createdDoor && createdDoor->getDecksHigh() == 2
				&& createdDoor->getSize().y > 1.0f,
			"Ordinary Door creation did not carry the two-deck span");

		// Past the authored ceiling nothing is accepted, authored or replayed.
		core::Building::CreateDoorOptions tooTall;
		tooTall.width = 1;
		tooTall.decksHigh = 3;
		std::string diagnostic;
		require(!original.canAddCorridorDoor(0, 0, 5, tooTall, &diagnostic),
			"A three-deck Door was accepted");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("decksHigh: 2") != std::string::npos,
			"Building YAML did not persist the Door's two-deck span");

		core::Building loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData),
			"Two-deck Door building did not round-trip");
		auto const loadedDoor = findDoor(loaded, created.door.sector->getIndex());
		require(loadedDoor && loadedDoor->getDecksHigh() == 2,
			"Loaded Door did not restore its two-deck span");

		// A Door record with no decksHigh field is a pre-span record.
		core::Building legacyOriginal("Legacy door", 8, 3);
		legacyOriginal.addRoom("Fore room", 0, 0, 0, 7, 2);
		legacyOriginal.addRoom("Back room", 1, 0, 0, 7, 2);
		auto const legacyCreated = legacyOriginal.addSectorDoor(0, 0, 3);
		legacyOriginal.finishBuild();
		core::SerializationWorkData legacyWork;
		auto legacyWriter = core::YamlSerializer::toString();
		legacyOriginal.serialize(*legacyWriter, legacyWork);
		legacyWriter->serialize();
		auto legacyYaml = legacyWriter->getSerializedString();
		auto const doorAt = legacyYaml.find("type: door");
		require(doorAt != std::string::npos, "No Door record in the legacy YAML fixture");
		auto const spanAt = legacyYaml.find("decksHigh:", doorAt);
		require(spanAt != std::string::npos, "The Door record carried no decksHigh field");
		auto const lineStart = legacyYaml.rfind('\n', spanAt) + 1;
		auto const lineEnd = legacyYaml.find('\n', spanAt);
		legacyYaml.erase(lineStart, lineEnd - lineStart + 1);
		core::Building legacy("placeholder", 1, 1);
		auto legacyReader = core::YamlSerializer::fromString(legacyYaml);
		legacyReader->deserialize();
		require(legacy.deserialize(*legacyReader, legacyWork),
			"A Door record without decksHigh did not load");
		auto const legacyDoor = findDoor(legacy, legacyCreated.door.sector->getIndex());
		require(legacyDoor && legacyDoor->getDecksHigh() == 1,
			"A pre-span Door record did not replay as one deck tall");
	}

	// Ticket #82: OpenLeft is a fully-fledged authored Door opening style. It is
	// carried by creation options, persisted as openStyle: openLeft, replayed on
	// load, editable through the selected-Door editor's Building call (which moves
	// the authored record and the live Door together), preserved by moves, and
	// rides the snapshot-based undo/redo and the option-based clipboard copy/paste.
	void doorOpenLeftPersistsThroughEveryEditorPath()
	{
		auto findDoor = [](core::Building const& building, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = building.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};

		core::Building original("OpenLeft door", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenLeft;
		auto const created = original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		auto const doorSectorIndex = created.door.sector->getIndex();
		auto const createdDoor = findDoor(original, doorSectorIndex);
		require(createdDoor && createdDoor->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"Ordinary Door creation did not carry the OpenLeft opening style");
		core::Building::CreateDoorOptions readBack;
		require(original.getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Authored Door options did not report the OpenLeft opening style");

		// Save/load: the record persists the style and replays it on load.
		auto const yaml = snapshotYaml(original);
		require(yaml.find("openStyle: openLeft") != std::string::npos,
			"Building YAML did not persist the Door's openStyle: openLeft");
		auto loaded = loadYaml(yaml);
		auto const loadedDoor = findDoor(*loaded, doorSectorIndex);
		require(loadedDoor && loadedDoor->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"Loaded Door lost its OpenLeft opening style");
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Loaded Door options lost the OpenLeft opening style");

		// The selected-Door editor's change: record and live Door move together.
		loaded->pauseSimulation();
		std::string diagnostic;
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenUp, &diagnostic),
			("Re-authoring the Door to OpenUp was refused: " + diagnostic).c_str());
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenUp,
			"Re-authored Door record did not take the new opening style");
		require(findDoor(*loaded, doorSectorIndex)->getOpenStyle() == core::Door::OpenStyle::OpenUp,
			"Re-authored live Door did not take the new opening style");
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Re-authoring the Door back to OpenLeft was refused: " + diagnostic).c_str());

		// Undo/redo mirror: the editor snapshots YAML before and after a change
		// and restores either side verbatim. Both sides carry their own style.
		auto const openLeftSnapshot = snapshotYaml(*loaded);
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenUp, &diagnostic),
			("Second re-author to OpenUp was refused: " + diagnostic).c_str());
		auto const openUpSnapshot = snapshotYaml(*loaded);
		auto const undone = loadYaml(openUpSnapshot);
		require(undone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenUp,
			"Undo snapshot did not restore the OpenUp style");
		auto const redone = loadYaml(openLeftSnapshot);
		require(redone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Redo snapshot did not restore the OpenLeft style");
		require(redone->setSectorDoorOpenStyle(0, 0, 9, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic) == false,
			"Opening-style edit was accepted where no Door record exists");

		// A move replays the authored record at a new position; the style rides along.
		auto& moveTarget = *redone;
		moveTarget.pauseSimulation();
		uint32_t movedDoorObjectIndex{ ~0u };
		{
			auto const sector = moveTarget.getSector(doorSectorIndex);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const candidate = sector->getObject(i);
				if (candidate && candidate->getObjectType() == core::SectorObjectType::Door
					&& static_pointer_cast<const core::DoorSectorObject>(candidate)->getDoor()
						== findDoor(moveTarget, doorSectorIndex))
					{ movedDoorObjectIndex = i; break; }
			}
		}
		require(movedDoorObjectIndex != ~0u, "OpenLeft Door object could not be found");
		auto const movePlan = moveTarget.planMoveSectorObject(
			doorSectorIndex, movedDoorObjectIndex, 5, 0);
		require(movePlan.valid, "OpenLeft Door move plan was rejected");
		require(moveTarget.applyObjectMove(movePlan) != nullptr, "OpenLeft Door move failed");
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Moving the OpenLeft Door did not preserve its opening style");

		// Clipboard copy/paste mirror: the paste side reads the copied Door's
		// authored options and re-creates through addSectorDoor; the style rides
		// through CreateDoorOptions unchanged.
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack),
			"Copied Door options could not be read");
		core::Building pasteTarget("OpenLeft paste", 8, 3);
		pasteTarget.addRoom("Fore room", 0, 0, 0, 7, 2);
		pasteTarget.addRoom("Back room", 1, 0, 0, 7, 2);
		auto const pasted = pasteTarget.addSectorDoor(0, 0, 1, readBack);
		pasteTarget.finishBuild();
		auto const pastedDoor = findDoor(pasteTarget, pasted.door.sector->getIndex());
		require(pastedDoor && pastedDoor->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"Pasted Door did not carry the copied OpenLeft opening style");
		require(pasteTarget.getSectorDoorOptions(0, 0, 1, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Pasted Door record lost the OpenLeft opening style");
	}

	// Ticket #83: OpenRight is the mirror-image sibling of OpenLeft and rides the
	// very same authored-style pipeline - creation options, the openStyle:
	// openRight record, load replay, the selected-Door editor's Building call
	// (record and live Door moving together), moves, snapshot-based undo/redo,
	// and option-based clipboard copy/paste. The checks move between OpenRight
	// and OpenLeft rather than OpenUp so the two horizontal styles are proven
	// distinct at every step.
	void doorOpenRightPersistsThroughEveryEditorPath()
	{
		auto findDoor = [](core::Building const& building, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = building.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};

		core::Building original("OpenRight door", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenRight;
		auto const created = original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		auto const doorSectorIndex = created.door.sector->getIndex();
		auto const createdDoor = findDoor(original, doorSectorIndex);
		require(createdDoor && createdDoor->getOpenStyle() == core::Door::OpenStyle::OpenRight,
			"Ordinary Door creation did not carry the OpenRight opening style");
		core::Building::CreateDoorOptions readBack;
		require(original.getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenRight,
			"Authored Door options did not report the OpenRight opening style");

		// Save/load: the record persists the style and replays it on load.
		auto const yaml = snapshotYaml(original);
		require(yaml.find("openStyle: openRight") != std::string::npos,
			"Building YAML did not persist the Door's openStyle: openRight");
		auto loaded = loadYaml(yaml);
		auto const loadedDoor = findDoor(*loaded, doorSectorIndex);
		require(loadedDoor && loadedDoor->getOpenStyle() == core::Door::OpenStyle::OpenRight,
			"Loaded Door lost its OpenRight opening style");
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenRight,
			"Loaded Door options lost the OpenRight opening style");

		// The selected-Door editor's change: record and live Door move together,
		// and OpenRight is never confused with its OpenLeft neighbour.
		loaded->pauseSimulation();
		std::string diagnostic;
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Re-authoring the Door to OpenLeft was refused: " + diagnostic).c_str());
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Re-authored Door record did not take the OpenLeft style");
		require(findDoor(*loaded, doorSectorIndex)->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"Re-authored live Door did not take the OpenLeft style");
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("Re-authoring the Door back to OpenRight was refused: " + diagnostic).c_str());
		require(findDoor(*loaded, doorSectorIndex)->getOpenStyle() == core::Door::OpenStyle::OpenRight,
			"Re-authored live Door did not return to OpenRight");

		// Undo/redo mirror: the editor snapshots YAML before and after a change
		// and restores either side verbatim. Both sides carry their own style.
		auto const openRightSnapshot = snapshotYaml(*loaded);
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Second re-author to OpenLeft was refused: " + diagnostic).c_str());
		auto const openLeftSnapshot = snapshotYaml(*loaded);
		auto const undone = loadYaml(openLeftSnapshot);
		require(undone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Undo snapshot did not restore the OpenLeft style");
		auto const redone = loadYaml(openRightSnapshot);
		require(redone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenRight,
			"Redo snapshot did not restore the OpenRight style");
		require(redone->setSectorDoorOpenStyle(0, 0, 9, 2,
			core::Door::OpenStyle::OpenRight, &diagnostic) == false,
			"OpenRight style edit was accepted where no Door record exists");

		// A move replays the authored record at a new position; the style rides along.
		auto& moveTarget = *redone;
		moveTarget.pauseSimulation();
		uint32_t movedDoorObjectIndex{ ~0u };
		{
			auto const sector = moveTarget.getSector(doorSectorIndex);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const candidate = sector->getObject(i);
				if (candidate && candidate->getObjectType() == core::SectorObjectType::Door
					&& static_pointer_cast<const core::DoorSectorObject>(candidate)->getDoor()
						== findDoor(moveTarget, doorSectorIndex))
					{ movedDoorObjectIndex = i; break; }
			}
		}
		require(movedDoorObjectIndex != ~0u, "OpenRight Door object could not be found");
		auto const movePlan = moveTarget.planMoveSectorObject(
			doorSectorIndex, movedDoorObjectIndex, 5, 0);
		require(movePlan.valid, "OpenRight Door move plan was rejected");
		require(moveTarget.applyObjectMove(movePlan) != nullptr, "OpenRight Door move failed");
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenRight,
			"Moving the OpenRight Door did not preserve its opening style");

		// Clipboard copy/paste mirror: the paste side reads the copied Door's
		// authored options and re-creates through addSectorDoor; the style rides
		// through CreateDoorOptions unchanged.
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack),
			"Copied Door options could not be read");
		core::Building pasteTarget("OpenRight paste", 8, 3);
		pasteTarget.addRoom("Fore room", 0, 0, 0, 7, 2);
		pasteTarget.addRoom("Back room", 1, 0, 0, 7, 2);
		auto const pasted = pasteTarget.addSectorDoor(0, 0, 1, readBack);
		pasteTarget.finishBuild();
		auto const pastedDoor = findDoor(pasteTarget, pasted.door.sector->getIndex());
		require(pastedDoor && pastedDoor->getOpenStyle() == core::Door::OpenStyle::OpenRight,
			"Pasted Door did not carry the copied OpenRight opening style");
		require(pasteTarget.getSectorDoorOptions(0, 0, 1, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenRight,
			"Pasted Door record lost the OpenRight opening style");
	}

	// Ticket #84: OpenApart is the fourth authored Door opening style and rides
	// the same authored-style pipeline as its siblings - creation options, the
	// openStyle: openApart record, load replay, the selected-Door editor's
	// Building call (record and live Door moving together), moves, snapshot-based
	// undo/redo, and option-based clipboard copy/paste. The checks move between
	// OpenApart and OpenLeft rather than OpenUp so the two horizontal styles are
	// proven distinct at every step.
	void doorOpenApartPersistsThroughEveryEditorPath()
	{
		auto findDoor = [](core::Building const& building, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = building.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};

		core::Building original("OpenApart door", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenApart;
		auto const created = original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		auto const doorSectorIndex = created.door.sector->getIndex();
		auto const createdDoor = findDoor(original, doorSectorIndex);
		require(createdDoor && createdDoor->getOpenStyle() == core::Door::OpenStyle::OpenApart,
			"Ordinary Door creation did not carry the OpenApart opening style");
		core::Building::CreateDoorOptions readBack;
		require(original.getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenApart,
			"Authored Door options did not report the OpenApart opening style");

		// Save/load: the record persists the style and replays it on load.
		auto const yaml = snapshotYaml(original);
		require(yaml.find("openStyle: openApart") != std::string::npos,
			"Building YAML did not persist the Door's openStyle: openApart");
		auto loaded = loadYaml(yaml);
		auto const loadedDoor = findDoor(*loaded, doorSectorIndex);
		require(loadedDoor && loadedDoor->getOpenStyle() == core::Door::OpenStyle::OpenApart,
			"Loaded Door lost its OpenApart opening style");
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenApart,
			"Loaded Door options lost the OpenApart opening style");

		// The selected-Door editor's change: record and live Door move together,
		// and OpenApart is never confused with its OpenLeft neighbour.
		loaded->pauseSimulation();
		std::string diagnostic;
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Re-authoring the Door to OpenLeft was refused: " + diagnostic).c_str());
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Re-authored Door record did not take the OpenLeft style");
		require(findDoor(*loaded, doorSectorIndex)->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"Re-authored live Door did not take the OpenLeft style");
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenApart, &diagnostic),
			("Re-authoring the Door back to OpenApart was refused: " + diagnostic).c_str());
		require(findDoor(*loaded, doorSectorIndex)->getOpenStyle() == core::Door::OpenStyle::OpenApart,
			"Re-authored live Door did not return to OpenApart");

		// Undo/redo mirror: the editor snapshots YAML before and after a change
		// and restores either side verbatim. Both sides carry their own style.
		auto const openApartSnapshot = snapshotYaml(*loaded);
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Second re-author to OpenLeft was refused: " + diagnostic).c_str());
		auto const openLeftSnapshot = snapshotYaml(*loaded);
		auto const undone = loadYaml(openLeftSnapshot);
		require(undone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Undo snapshot did not restore the OpenLeft style");
		auto const redone = loadYaml(openApartSnapshot);
		require(redone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenApart,
			"Redo snapshot did not restore the OpenApart style");
		require(redone->setSectorDoorOpenStyle(0, 0, 9, 2,
			core::Door::OpenStyle::OpenApart, &diagnostic) == false,
			"OpenApart style edit was accepted where no Door record exists");

		// A move replays the authored record at a new position; the style rides along.
		auto& moveTarget = *redone;
		moveTarget.pauseSimulation();
		uint32_t movedDoorObjectIndex{ ~0u };
		{
			auto const sector = moveTarget.getSector(doorSectorIndex);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const candidate = sector->getObject(i);
				if (candidate && candidate->getObjectType() == core::SectorObjectType::Door
					&& static_pointer_cast<const core::DoorSectorObject>(candidate)->getDoor()
						== findDoor(moveTarget, doorSectorIndex))
					{ movedDoorObjectIndex = i; break; }
			}
		}
		require(movedDoorObjectIndex != ~0u, "OpenApart Door object could not be found");
		auto const movePlan = moveTarget.planMoveSectorObject(
			doorSectorIndex, movedDoorObjectIndex, 5, 0);
		require(movePlan.valid, "OpenApart Door move plan was rejected");
		require(moveTarget.applyObjectMove(movePlan) != nullptr, "OpenApart Door move failed");
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenApart,
			"Moving the OpenApart Door did not preserve its opening style");

		// Clipboard copy/paste mirror: the paste side reads the copied Door's
		// authored options and re-creates through addSectorDoor; the style rides
		// through CreateDoorOptions unchanged.
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack),
			"Copied Door options could not be read");
		core::Building pasteTarget("OpenApart paste", 8, 3);
		pasteTarget.addRoom("Fore room", 0, 0, 0, 7, 2);
		pasteTarget.addRoom("Back room", 1, 0, 0, 7, 2);
		auto const pasted = pasteTarget.addSectorDoor(0, 0, 1, readBack);
		pasteTarget.finishBuild();
		auto const pastedDoor = findDoor(pasteTarget, pasted.door.sector->getIndex());
		require(pastedDoor && pastedDoor->getOpenStyle() == core::Door::OpenStyle::OpenApart,
			"Pasted Door did not carry the copied OpenApart opening style");
		require(pasteTarget.getSectorDoorOptions(0, 0, 1, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenApart,
			"Pasted Door record lost the OpenApart opening style");
	}

	// Ticket #84: generated-Door defaults are owner-sensitive. A Lift's landing
	// Doors are authored OpenApart so a Lift entrance reads as a centre-opening
	// pair, while ordinary and Shuttle-owned Doors keep their established OpenUp.
	// The default lives with the generated Door rather than the transport's own
	// record, so a legacy Lift map - which carries no style data at all - still
	// reconstructs its landing Doors OpenApart.
	void liftDoorsDefaultToOpenApartWhileOtherDoorsKeepOpenUp()
	{
		auto doorFromResult = [](core::Building::CreateObjectResult const& result)
			-> std::shared_ptr<const core::Door>
		{
			if (!result.sector || result.index == ~0u) return nullptr;
			auto const object = result.sector->getObject(result.index);
			if (!object || object->getObjectType() != core::SectorObjectType::Door) return nullptr;
			return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
		};
		// Every Door of one ownership kind, found through the public ownership
		// predicates rather than by remembering where creation left it.
		auto collectOwnedDoorStyles = [](core::Building const& building, bool liftDoors)
		{
			std::vector<core::Door::OpenStyle> styles;
			std::set<core::Door const*> seen;
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, stopIndex{ ~0u }, carriageIndex{ ~0u };
					bool const owned = liftDoors
						? building.isLiftOwnedDoor(object, &ownerSector, &stopIndex)
						: building.isShuttleOwnedDoor(object, &ownerSector, &stopIndex, &carriageIndex);
					if (!owned) continue;
					auto const door = static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
					if (!seen.insert(door.get()).second) continue;
					styles.push_back(door->getOpenStyle());
				}
			}
			return styles;
		};
		auto requireAll = [](std::vector<core::Door::OpenStyle> const& styles,
			core::Door::OpenStyle expected, size_t atLeast, char const* what)
		{
			require(styles.size() >= atLeast,
				(std::string("Too few Doors were found to check: ") + what).c_str());
			for (auto const style : styles)
				require(style == expected, (std::string(what) + " has the wrong default opening style").c_str());
		};

		// An ordinary Door with no style set keeps the OpenUp default.
		core::Building ordinary("Ordinary door defaults", 12, 3);
		ordinary.addRoom("Fore", 0, 0, 0, 11, 2);
		ordinary.addRoom("Aft", 1, 0, 0, 11, 2);
		auto const ordinaryDoor = ordinary.addSectorDoor(0, 0, 3, core::Building::CreateDoorOptions{});
		ordinary.finishBuild();
		require(doorFromResult(ordinaryDoor.door)
			&& doorFromResult(ordinaryDoor.door)->getOpenStyle() == core::Door::OpenStyle::OpenUp,
			"An ordinary Door no longer defaults to OpenUp");

		// Every landing Door a Lift generates is authored OpenApart.
		core::Building liftBuilding("Lift door defaults", 16, 3);
		auto hall = liftBuilding.addRoom("Lift Hall", 0, 0, 0, 16, 3);
		for (uint32_t deck = 1; deck < 3; ++deck)
			for (uint32_t x = 0; x < 16; ++x)
				liftBuilding.addSectorWalkway(hall, deck, x);
		core::Building::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.decksHigh = 3;
		liftOptions.stopOffsets = { 0, 1, 2 };
		auto const lift = liftBuilding.addLift(1, 0, 8, liftOptions);
		liftBuilding.finishBuild();
		require(lift.doors.size() == 3, "The Lift did not generate one Door per stop");
		for (size_t i = 0; i < lift.doors.size(); ++i)
			require(doorFromResult(lift.doors[i].door)
				&& doorFromResult(lift.doors[i].door)->getOpenStyle() == core::Door::OpenStyle::OpenApart,
				"A newly created Lift Door does not default to OpenApart");
		requireAll(collectOwnedDoorStyles(liftBuilding, true), core::Door::OpenStyle::OpenApart, 3,
			"A Lift-owned Door");

		// The Lift record carries no style of its own, so replaying a legacy map -
		// whose Lift record predates opening styles entirely - still lands on the
		// generated-Door default rather than an explicit override.
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		liftBuilding.serialize(*writer, workData);
		writer->serialize();
		auto const liftYaml = writer->getSerializedString();
		require(liftYaml.find("type: lift") != std::string::npos,
			"The Lift record was not persisted");
		require(liftYaml.find("openStyle") == std::string::npos,
			"A Lift record grew its own opening style, so a legacy Lift map would no "
			"longer reconstruct through the generated-Door default");
		auto legacy = std::make_shared<core::Building>("placeholder", 1, 1);
		{
			auto reader = core::YamlSerializer::fromString(liftYaml);
			reader->deserialize();
			legacy->deserialize(*reader, workData);
		}
		requireAll(collectOwnedDoorStyles(*legacy, true), core::Door::OpenStyle::OpenApart, 3,
			"A reconstructed Lift Door");

		// Shuttle-owned Doors keep the OpenUp default: only Lift Doors change.
		core::Building shuttleBuilding("Shuttle door defaults", 32, 3);
		shuttleBuilding.addCorridor(0, 0, 31);
		shuttleBuilding.addCorridor(1, 0, 31);
		core::Building::CreateShuttleOptions shuttleOptions{ 2, 3, { 0, 18 }, 0 };
		shuttleOptions.capacity = 2;
		shuttleOptions.doorMask = 0b101;
		shuttleBuilding.addShuttle(1, 0, 0, 27, shuttleOptions);
		shuttleBuilding.finishBuild();
		requireAll(collectOwnedDoorStyles(shuttleBuilding, false), core::Door::OpenStyle::OpenUp, 2,
			"A Shuttle-owned Door");
	}

	// Ticket #100: a map carrying authored Door opening styles is written at
	// schema version 8, one above the version-6 ceiling of every pre-feature
	// build, so those builds refuse the whole file instead of accepting it and
	// erasing every style when they next save. Version 6 files keep loading,
	// replaying OpenUp for ordinary and Shuttle-owned Doors and OpenApart for
	// Lift-owned Doors when no style field is present.
	void doorStyleMapsAdvanceTheSchemaVersionAndLegacySixStillLoads()
	{
		auto findDoor = [](core::Building const& building, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = building.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};
		// Every Door of one ownership kind, found through the public ownership
		// predicates rather than by remembering where creation left it.
		auto collectOwnedDoorStyles = [](core::Building const& building, bool liftDoors)
		{
			std::vector<core::Door::OpenStyle> styles;
			std::set<core::Door const*> seen;
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, stopIndex{ ~0u }, carriageIndex{ ~0u };
					bool const owned = liftDoors
						? building.isLiftOwnedDoor(object, &ownerSector, &stopIndex)
						: building.isShuttleOwnedDoor(object, &ownerSector, &stopIndex, &carriageIndex);
					if (!owned) continue;
					auto const door = static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
					if (!seen.insert(door.get()).second) continue;
					styles.push_back(door->getOpenStyle());
				}
			}
			return styles;
		};
		auto requireAllStyles = [](std::vector<core::Door::OpenStyle> const& styles,
			core::Door::OpenStyle expected, size_t atLeast, char const* what)
		{
			require(styles.size() >= atLeast,
				(std::string("Too few Doors were found to check: ") + what).c_str());
			for (auto const style : styles)
				require(style == expected,
					(std::string(what) + " has the wrong opening style").c_str());
		};
		auto serialize = [](core::Building const& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};

		// A stand-in for a pre-Door-style (version 6) build's reader: the version
		// ceiling that build accepted, and its construction-record name table. A
		// style-bearing map has to be refused at the version check rather than
		// have its Door style fields silently ignored.
		uint32_t const preDoorStyleVersionCeiling{ 6 };
		static std::set<std::string> const preDoorStyleRecordNames{
			"corridor", "room", "ladder", "stairwell", "staircase", "lift", "shuttle", "door",
			"window", "bulkheadDoor", "lightSwitch", "forceBridge", "sectorLadder", "platformLift",
			"walkway", "marker", "removeWall", "removeMarker", "objectTombstone", "background",
			"facade" };
		auto preDoorStyleReaderReads = [&](std::string const& yaml)
		{
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			reader->beginMap("building");
			auto const version = reader->readUint32("version");
			if (version > preDoorStyleVersionCeiling)
				throw core::SerializationException("Unsupported Building serialization version");
			reader->beginArray("construction");
			while (reader->nextArrayItem())
			{
				reader->beginMap("");
				auto const type = reader->readString("type");
				if (preDoorStyleRecordNames.find(type) == preDoorStyleRecordNames.end())
					throw core::SerializationException(
						"Unknown Building construction record type: " + type);
				reader->endMap();
			}
			reader->endArray();
			reader->endMap();
		};

		// One map exercising every ownership kind and every style field: an
		// ordinary OpenLeft Door, a Lift with a per-stop override, and a Shuttle
		// with a per-Door override.
		core::Building authored("Door-style schema", 40, 3);
		authored.addRoom("Fore hall", 0, 0, 0, 8, 3);
		authored.addRoom("Aft room", 1, 0, 0, 8, 3);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenLeft;
		auto const ordinaryDoor = authored.addSectorDoor(0, 0, 3, doorOptions);
		auto const liftHall = authored.addRoom("Lift hall", 0, 0, 10, 8, 3);
		for (uint32_t deck = 1; deck < 3; ++deck)
			for (uint32_t x = 0; x < 8; ++x)
				authored.addSectorWalkway(liftHall, deck, x);
		core::Building::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.decksHigh = 3;
		liftOptions.stopOffsets = { 0, 1, 2 };
		auto const lift = authored.addLift(1, 0, 13, liftOptions);
		authored.addCorridor(0, 0, 20, 20, 1);
		core::Building::CreateShuttleOptions shuttleOptions{ 2, 3, { 0, 12 }, 0 };
		shuttleOptions.doorOpenStyles = { static_cast<uint32_t>(core::Door::OpenStyle::OpenApart),
			~0u, ~0u, ~0u };
		auto const shuttle = authored.addShuttle(1, 0, 20, 20, shuttleOptions);
		authored.finishBuild();
		require(lift.doors.size() == 3, "The Lift did not generate one Door per stop");
		require(shuttle.doors.size() == 4, "The Shuttle did not generate four landing Doors");
		authored.pauseSimulation();
		std::string diagnostic;
		require(authored.setLiftStopDoorOpenStyle(lift.lift.sector->getIndex(), 1,
				core::Door::OpenStyle::OpenRight, &diagnostic),
			("A Lift stop style override was refused: " + diagnostic).c_str());

		auto const yaml = serialize(authored);
		require(yaml.find("version: 8") != std::string::npos,
			"A map with authored Door styles was not written at version 8");
		require(yaml.find("version: 6") == std::string::npos,
			"A map with authored Door styles still carries version 6");
		require(yaml.find("openStyle: openLeft") != std::string::npos
			&& yaml.find("stopDoorOpenStyles") != std::string::npos
			&& yaml.find("doorOpenStyles") != std::string::npos,
			"The Door-style fields were not persisted alongside the new version");

		// A version-6 reader refuses the file at the version check: the refusal
		// is about the schema version, not about record shapes it would have
		// accepted.
		bool refusedVersion{ false };
		try
		{
			preDoorStyleReaderReads(yaml);
		}
		catch (core::SerializationException const& error)
		{
			refusedVersion = true;
			require(std::string(error.what()).find("version") != std::string::npos,
				("A version-6 reader failed for a reason other than the version: "
					+ std::string(error.what())).c_str());
		}
		require(refusedVersion, "A version-6 reader accepted a version-7 Door-style map");

		// A defaults-only map replays exactly like a pre-feature version-6 file:
		// every style field absent.  Reconstruct that legacy shape by rewriting
		// the version and dropping the only style line the writer emitted.
		core::Building defaults("Legacy-shaped map", 40, 3);
		defaults.addRoom("Fore hall", 0, 0, 0, 8, 3);
		defaults.addRoom("Aft room", 1, 0, 0, 8, 3);
		auto const defaultDoor = defaults.addSectorDoor(0, 0, 3, core::Building::CreateDoorOptions{});
		auto const defaultHall = defaults.addRoom("Lift hall", 0, 0, 10, 8, 3);
		for (uint32_t deck = 1; deck < 3; ++deck)
			for (uint32_t x = 0; x < 8; ++x)
				defaults.addSectorWalkway(defaultHall, deck, x);
		core::Building::CreateLiftOptions defaultLiftOptions;
		defaultLiftOptions.cellsWide = 1;
		defaultLiftOptions.decksHigh = 3;
		defaultLiftOptions.stopOffsets = { 0, 1, 2 };
		auto const defaultLift = defaults.addLift(1, 0, 13, defaultLiftOptions);
		defaults.addCorridor(0, 0, 20, 20, 1);
		core::Building::CreateShuttleOptions defaultShuttleOptions{ 2, 3, { 0, 12 }, 0 };
		auto const defaultShuttle = defaults.addShuttle(1, 0, 20, 20, defaultShuttleOptions);
		defaults.finishBuild();

		auto defaultsYaml = serialize(defaults);
		require(defaultsYaml.find("stopDoorOpenStyles") == std::string::npos
			&& defaultsYaml.find("doorOpenStyles") == std::string::npos,
			"A defaults-only map persisted transport style overrides");
		std::string const styleLine = "    openStyle: openUp\n";
		require(defaultsYaml.find(styleLine) != std::string::npos,
			"The defaults-only map did not persist the ordinary Door's openStyle line");
		auto const legacyYaml = std::string("version: 6")
			+ defaultsYaml.substr(defaultsYaml.find("\n"));
		auto const strippedYaml = legacyYaml.substr(0, legacyYaml.find(styleLine))
			+ legacyYaml.substr(legacyYaml.find(styleLine) + styleLine.size());
		require(strippedYaml.find("openStyle") == std::string::npos,
			"The reconstructed legacy map still carried a Door style field");

		// The version-6 reader is content with the legacy shape: its refusal of
		// the new map is the version alone.
		preDoorStyleReaderReads(strippedYaml);

		// The current reader loads version 6 and supplies the owner-sensitive
		// defaults: OpenUp for the ordinary and Shuttle-owned Doors, OpenApart
		// for the Lift-owned Doors.
		auto legacy = std::make_shared<core::Building>("placeholder", 1, 1);
		{
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(strippedYaml);
			reader->deserialize();
			legacy->deserialize(*reader, workData);
		}
		auto const legacyOrdinary = findDoor(*legacy, defaultDoor.door.sector->getIndex());
		require(legacyOrdinary
			&& legacyOrdinary->getOpenStyle() == core::Door::OpenStyle::OpenUp,
			"A legacy ordinary Door did not replay as OpenUp");
		requireAllStyles(collectOwnedDoorStyles(*legacy, true), core::Door::OpenStyle::OpenApart, 3,
			"A legacy Lift-owned Door");
		requireAllStyles(collectOwnedDoorStyles(*legacy, false), core::Door::OpenStyle::OpenUp, 4,
			"A legacy Shuttle-owned Door");

		// The current reader still refuses anything above its own ceiling.
		auto const futureYaml = std::string("version: 9")
			+ defaultsYaml.substr(defaultsYaml.find("\n"));
		bool refusedFuture{ false };
		try
		{
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(futureYaml);
			reader->deserialize();
			core::Building rejected("placeholder", 1, 1);
			rejected.deserialize(*reader, workData);
		}
		catch (core::SerializationException const&)
		{
			refusedFuture = true;
		}
		require(refusedFuture, "A version-8 map was accepted by the current reader");
	}

	// Ticket #85: each Door at a Lift stop carries an individually authored
	// opening-style override while the Lift's topology stays fixed. Editing one
	// stop changes no sibling, the overrides round-trip through Building
	// persistence, snapshot-based undo/redo restores the previous per-stop
	// style, and reconstructing an unchanged Lift retains every override.
	void liftStopDoorStyleOverridesArePerStopAndPersist()
	{
		auto findLiftStopDoor = [](core::Building const& building, uint32_t liftSector,
			uint32_t stopIndex) -> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					if (building.isLiftOwnedDoor(object, &ownerSector, &ownerStop)
						&& ownerSector == liftSector && ownerStop == stopIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};

		core::Building building("Lift stop door styles", 12, 3);
		building.addRoom("Landing A", 0, 0, 0, 12, 1);
		building.addRoom("Landing B", 0, 1, 0, 12, 1);
		building.addRoom("Landing C", 0, 2, 0, 12, 1);
		auto const created = building.addLift(1, 0, 2, 1, 3);
		building.finishBuild();
		require(created.doors.size() == 3, "The Lift did not generate three landing Doors");
		auto const liftSector = created.lift.sector->getIndex();
		building.pauseSimulation();

		// A Lift with no per-stop choices persists no style data at all, and an
		// override outside the authored topology is refused.
		require(snapshotYaml(building).find("stopDoorOpenStyles") == std::string::npos,
			"A Lift with no per-stop overrides persisted a stopDoorOpenStyles array");
		std::string diagnostic;
		require(!building.setLiftStopDoorOpenStyle(liftSector, 3,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			"An override was accepted for a stop outside the Lift's authored topology");

		// Editing one stop takes effect live and touches no sibling stop.
		require(building.setLiftStopDoorOpenStyle(liftSector, 1,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("A per-stop override was refused: " + diagnostic).c_str());
		require(findLiftStopDoor(building, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"The edited Lift stop Door did not take the override live");
		require(findLiftStopDoor(building, liftSector, 0)->getOpenStyle()
				== core::Door::OpenStyle::OpenApart
			&& findLiftStopDoor(building, liftSector, 2)->getOpenStyle()
				== core::Door::OpenStyle::OpenApart,
			"Editing one Lift stop Door changed a sibling stop");

		// A second stop takes its own style while the first keeps its own.
		require(building.setLiftStopDoorOpenStyle(liftSector, 2,
			core::Door::OpenStyle::OpenUp, &diagnostic),
			("A second per-stop override was refused: " + diagnostic).c_str());
		require(findLiftStopDoor(building, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"Editing a second Lift stop changed the first stop's override");

		// Save/load: the overrides ride in the Lift's own record, and the stop
		// without an override replays as the generated OpenApart default.
		auto const yaml = snapshotYaml(building);
		require(yaml.find("stopDoorOpenStyles") != std::string::npos,
			"The Lift record did not persist its per-stop Door styles");
		require(yaml.find("openLeft") != std::string::npos
			&& yaml.find("openUp") != std::string::npos
			&& yaml.find("default") != std::string::npos,
			"The persisted stopDoorOpenStyles array lost a stop's style");
		auto loaded = loadYaml(yaml);
		require(findLiftStopDoor(*loaded, liftSector, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenApart,
			"A loaded Lift stop without an override lost the OpenApart default");
		require(findLiftStopDoor(*loaded, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"A loaded Lift stop lost its OpenLeft override");
		require(findLiftStopDoor(*loaded, liftSector, 2)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"A loaded Lift stop lost its OpenUp override");

		// Undo/redo mirror: the editor snapshots YAML before and after a change
		// and restores either side verbatim.
		loaded->pauseSimulation();
		auto const before = snapshotYaml(*loaded);
		require(loaded->setLiftStopDoorOpenStyle(liftSector, 1,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("A re-author to OpenRight was refused: " + diagnostic).c_str());
		auto const after = snapshotYaml(*loaded);
		require(findLiftStopDoor(*loaded, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"The re-authored stop did not take OpenRight live");
		auto const undone = loadYaml(before);
		require(findLiftStopDoor(*undone, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"Undo did not restore the previous per-stop style");
		require(findLiftStopDoor(*undone, liftSector, 2)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Undo disturbed another stop's override");
		auto const redone = loadYaml(after);
		require(findLiftStopDoor(*redone, liftSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"Redo did not restore the edited per-stop style");

		// Reconstructing the Lift with unchanged topology retains every override:
		// the edit plan rewrites the record in place and the overrides follow
		// their stops.
		redone->pauseSimulation();
		auto const rebuildPlan = redone->planResizeLift(liftSector, 2, 0, 1, 3);
		require(rebuildPlan.valid,
			("An unchanged-topology Lift rebuild plan was refused: "
				+ rebuildPlan.diagnostic).c_str());
		require(rebuildPlan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2 },
			"The rebuild plan did not keep the Lift's stop topology");
		auto const rebuiltSector = redone->applyLiftEdit(rebuildPlan);
		require(rebuiltSector == liftSector, "The rebuilt Lift moved to another Sector");
		require(findLiftStopDoor(*redone, rebuiltSector, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenApart,
			"Reconstructing the Lift lost the no-override OpenApart default");
		require(findLiftStopDoor(*redone, rebuiltSector, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"Reconstructing an unchanged Lift lost a per-stop override");
		require(findLiftStopDoor(*redone, rebuiltSector, 2)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Reconstructing an unchanged Lift lost a second per-stop override");
	}

	// Ticket #101: per-stop Door styles supplied through CreateLiftOptions at
	// creation time are authored Lift data.  They reach the live Doors, ride in
	// the Lift's construction record, round-trip through save/load unchanged,
	// and survive an unchanged rebuild and a stop-preserving resize on their
	// stop identities.
	void liftCreationStopDoorStylesAreAuthoredAndPersist()
	{
		auto findLiftStopDoor = [](core::Building const& building, uint32_t liftSector,
			uint32_t stopIndex) -> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					if (building.isLiftOwnedDoor(object, &ownerSector, &ownerStop)
						&& ownerSector == liftSector && ownerStop == stopIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto requireStyles = [&findLiftStopDoor](core::Building const& building, uint32_t liftSector,
			char const* context)
		{
			auto const expected = { core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenApart,
				core::Door::OpenStyle::OpenUp };
			uint32_t stop = 0;
			for (auto const style : expected)
			{
				auto const door = findLiftStopDoor(building, liftSector, stop);
				require(static_cast<bool>(door),
					(std::string(context) + ": landing Door is missing").c_str());
				require(door->getOpenStyle() == style,
					(std::string(context) + ": stop " + std::to_string(stop)
						+ " does not carry its creation-time style").c_str());
				++stop;
			}
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};

		// Landings live on floors 1-3 so extending the shaft downward shifts
		// every stop offset while retaining the same stop floors.
		core::Building building("Lift creation styles", 12, 4);
		building.addRoom("Landing 1", 0, 1, 0, 12, 1);
		building.addRoom("Landing 2", 0, 2, 0, 12, 1);
		building.addRoom("Landing 3", 0, 3, 0, 12, 1);
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		options.decksHigh = 3;
		options.stopOffsets = { 0, 1, 2 };
		// The middle stop carries no override (~0u) and must keep the generated
		// OpenApart default through every replay.
		options.stopDoorOpenStyles = { static_cast<uint32_t>(core::Door::OpenStyle::OpenLeft),
			~0u, static_cast<uint32_t>(core::Door::OpenStyle::OpenUp) };
		auto const created = building.addLift(1, 1, 2, options);
		building.finishBuild();
		building.pauseSimulation();
		require(created.doors.size() == 3, "The Lift did not generate three landing Doors");
		auto const liftSector = created.lift.sector->getIndex();
		requireStyles(building, liftSector, "Lift styled at creation");

		// The styles are authored record data, so they reach the persistence
		// boundary immediately rather than only the initial Door objects.
		auto const yaml = snapshotYaml(building);
		require(yaml.find("stopDoorOpenStyles") != std::string::npos,
			"A Lift styled at creation did not persist its stopDoorOpenStyles array");
		require(yaml.find("openLeft") != std::string::npos
			&& yaml.find("openUp") != std::string::npos
			&& yaml.find("default") != std::string::npos,
			"The persisted stopDoorOpenStyles array lost a creation-time style");

		// Save/load: every style, including the explicit no-override default,
		// replays onto the same stop.
		auto loaded = loadYaml(yaml);
		requireStyles(*loaded, liftSector, "Loaded Lift styled at creation");

		// An unchanged rebuild retains the creation-time styles on their stops.
		loaded->pauseSimulation();
		auto const rebuildPlan = loaded->planResizeLift(liftSector, 2, 1, 1, 3);
		require(rebuildPlan.valid,
			("An unchanged-topology rebuild plan was refused: " + rebuildPlan.diagnostic).c_str());
		require(rebuildPlan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2 },
			"The rebuild plan did not keep the Lift's stop topology");
		auto const rebuiltSector = loaded->applyLiftEdit(rebuildPlan);
		require(rebuiltSector == liftSector, "The rebuilt Lift moved to another Sector");
		requireStyles(*loaded, liftSector, "Rebuilt Lift styled at creation");

		// A stop-preserving resize - the shaft extends below its stops, shifting
		// every offset - keeps each style on its stop's landing floor.
		auto const resizePlan = loaded->planResizeLift(liftSector, 2, 0, 1, 4);
		require(resizePlan.valid,
			("A stop-preserving resize plan was refused: " + resizePlan.diagnostic).c_str());
		require(resizePlan.stopOffsets == std::vector<uint32_t>{ 1, 2, 3 },
			"The resize plan did not shift the stop offsets as expected");
		auto const resizedSector = loaded->applyLiftEdit(resizePlan);
		require(resizedSector == liftSector, "The resized Lift moved to another Sector");
		requireStyles(*loaded, liftSector, "Resized Lift styled at creation");

		// The reconciled styles still round-trip after the resize.
		auto const resized = loadYaml(snapshotYaml(*loaded));
		requireStyles(*resized, liftSector, "Loaded Lift after a stop-preserving resize");
	}

	// Ticket #105: editing a stop on a Lift whose creation/load override vector
	// is shorter than the stop list must not erase the accepted overrides on the
	// earlier stops.  The setter normalizes with a preserving resize, so every
	// existing entry survives and only the newly added slots take the default
	// sentinel.
	void liftShortStopDoorStyleVectorEditPreservesEarlierOverrides()
	{
		auto findLiftStopDoor = [](core::Building const& building, uint32_t liftSector,
			uint32_t stopIndex) -> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					if (building.isLiftOwnedDoor(object, &ownerSector, &ownerStop)
						&& ownerSector == liftSector && ownerStop == stopIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto requireStyles = [&findLiftStopDoor](core::Building const& building, uint32_t liftSector,
			std::vector<core::Door::OpenStyle> const& styles, char const* context)
		{
			for (size_t stop = 0; stop < styles.size(); ++stop)
			{
				auto const door = findLiftStopDoor(building, liftSector, (uint32_t)stop);
				require(static_cast<bool>(door),
					(std::string(context) + ": landing Door is missing").c_str());
				require(door->getOpenStyle() == styles[stop],
					(std::string(context) + ": stop " + std::to_string(stop)
						+ " does not carry its expected style").c_str());
			}
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};
		auto const expectedAfterEdit = { core::Door::OpenStyle::OpenLeft,
			core::Door::OpenStyle::OpenApart, core::Door::OpenStyle::OpenRight };

		core::Building building("Lift short style vector edit", 12, 4);
		building.addRoom("Landing 1", 0, 1, 0, 12, 1);
		building.addRoom("Landing 2", 0, 2, 0, 12, 1);
		building.addRoom("Landing 3", 0, 3, 0, 12, 1);
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		options.decksHigh = 3;
		options.stopOffsets = { 0, 1, 2 };
		// A creation-time vector shorter than the stop list: only stop 0 is
		// styled; stops 1 and 2 replay with the generated OpenApart default.
		options.stopDoorOpenStyles = { static_cast<uint32_t>(core::Door::OpenStyle::OpenLeft) };
		auto const created = building.addLift(1, 1, 2, options);
		building.finishBuild();
		building.pauseSimulation();
		require(created.doors.size() == 3, "The Lift did not generate three landing Doors");
		auto const liftSector = created.lift.sector->getIndex();
		requireStyles(building, liftSector,
			{ core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenApart,
				core::Door::OpenStyle::OpenApart },
			"Lift created with a one-entry style vector");

		// The creation-time document persists the short array as written by
		// addLift: a single openLeft entry with no padding.
		auto const shortYaml = snapshotYaml(building);
		require(shortYaml.find("stopDoorOpenStyles") != std::string::npos,
			"The short creation-time style vector was not persisted");

		// Editing a later stop preserves the creation-time override on stop 0
		// and default-fills only the slots that had no authored style.
		std::string diagnostic;
		require(building.setLiftStopDoorOpenStyle(liftSector, 2,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("A per-stop override on a short-vector Lift was refused: " + diagnostic).c_str());
		requireStyles(building, liftSector, expectedAfterEdit,
			"Live Lift after editing stop 2 of a short-vector Lift");
		auto const editedYaml = snapshotYaml(building);
		require(editedYaml.find("openLeft") != std::string::npos
			&& editedYaml.find("openRight") != std::string::npos
			&& editedYaml.find("default") != std::string::npos,
			"The persisted stopDoorOpenStyles array lost the stop 0 override after editing stop 2");

		// The load path: loading the short-array document and editing a later
		// stop goes through the same normalization without erasing the loaded
		// prefix entry.
		auto loaded = loadYaml(shortYaml);
		loaded->pauseSimulation();
		require(loaded->setLiftStopDoorOpenStyle(liftSector, 2,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("A per-stop override on a loaded short-vector Lift was refused: " + diagnostic).c_str());
		requireStyles(*loaded, liftSector, expectedAfterEdit,
			"Loaded Lift after editing stop 2 with a short override vector");

		// The corrected state round-trips through save/load unchanged.
		auto const reloaded = loadYaml(snapshotYaml(*loaded));
		requireStyles(*reloaded, liftSector, expectedAfterEdit,
			"Reloaded Lift after the corrected edit");

		// An unchanged rebuild retains the corrected result.
		reloaded->pauseSimulation();
		auto const rebuildPlan = reloaded->planResizeLift(liftSector, 2, 1, 1, 3);
		require(rebuildPlan.valid,
			("An unchanged-topology rebuild plan was refused: " + rebuildPlan.diagnostic).c_str());
		require(rebuildPlan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2 },
			"The rebuild plan did not keep the Lift's stop topology");
		auto const rebuiltSector = reloaded->applyLiftEdit(rebuildPlan);
		require(rebuiltSector == liftSector, "The rebuilt Lift moved to another Sector");
		requireStyles(*reloaded, liftSector, expectedAfterEdit,
			"Unchanged rebuild of the corrected Lift");
	}

	// Ticket #86: per-stop Door styles follow the stop's identity when the Lift
	// moves or resizes without changing its stop floors. The overrides remap by
	// absolute landing floor rather than by the transient offset from the shaft
	// anchor, so a sideways move, a shaft extension that shifts every offset,
	// and a width change all retain each stop's own style, and the reconciled
	// styles survive save/load.
	void liftDoorStylesFollowStopsWhenTheLiftMovesOrResizes()
	{
		auto findLiftStopDoor = [](core::Building const& building, uint32_t liftSector,
			uint32_t stopIndex) -> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					if (building.isLiftOwnedDoor(object, &ownerSector, &ownerStop)
						&& ownerSector == liftSector && ownerStop == stopIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto stopFloor = [](core::Building const& building, uint32_t liftSector,
			uint32_t stopIndex) -> uint32_t
		{
			auto const lift = std::dynamic_pointer_cast<const core::LiftTransit>(
				building.getSector(liftSector));
			require(static_cast<bool>(lift), "The Lift transit disappeared");
			auto const& value = lift->getStop(stopIndex);
			return static_cast<uint32_t>((int)value.sector->getCellY() + value.sectorOffsetY);
		};
		auto requireStyles = [&findLiftStopDoor](core::Building const& building, uint32_t liftSector,
			std::vector<core::Door::OpenStyle> const& styles, char const* context)
		{
			for (size_t stop = 0; stop < styles.size(); ++stop)
			{
				auto const door = findLiftStopDoor(building, liftSector, (uint32_t)stop);
				require(static_cast<bool>(door),
					(std::string(context) + ": landing Door is missing").c_str());
				require(door->getOpenStyle() == styles[stop],
					(std::string(context) + ": stop " + std::to_string(stop)
						+ " does not carry its own style").c_str());
			}
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};

		// Landings live on floors 1-3 so the shaft's bottom anchor never sits on
		// a stop: extending the shaft downward shifts every stop offset while
		// retaining the same stop floors.
		core::Building building("Lift move style retention", 12, 4);
		building.addRoom("Landing 1", 0, 1, 0, 12, 1);
		building.addRoom("Landing 2", 0, 2, 0, 12, 1);
		building.addRoom("Landing 3", 0, 3, 0, 12, 1);
		auto const created = building.addLift(1, 1, 2, 1, 3);
		building.finishBuild();
		building.pauseSimulation();
		require(created.doors.size() == 3, "The Lift did not generate three landing Doors");
		auto liftSector = created.lift.sector->getIndex();
		std::string diagnostic;
		const std::vector<core::Door::OpenStyle> styles{
			core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenUp,
			core::Door::OpenStyle::OpenRight };
		for (uint32_t stop = 0; stop < 3; ++stop)
			require(building.setLiftStopDoorOpenStyle(liftSector, stop, styles[stop], &diagnostic),
				("A per-stop override was refused: " + diagnostic).c_str());
		requireStyles(building, liftSector, styles, "Freshly styled Lift");

		// A sideways move retains the stop set and every style.
		{
			auto const plan = building.planResizeLift(liftSector, 6, 1, 1, 3);
			require(plan.valid, ("A sideways Lift move was refused: " + plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2 },
				"The sideways move did not retain the stop offsets");
			liftSector = building.applyLiftEdit(plan);
			require(stopFloor(building, liftSector, 0) == 1
				&& stopFloor(building, liftSector, 1) == 2
				&& stopFloor(building, liftSector, 2) == 3,
				"The sideways move changed the stop floors");
			requireStyles(building, liftSector, styles, "Lift moved sideways");
		}

		// Extending the shaft downward keeps the same stop floors but shifts
		// every stop offset by one; each style must follow its stop's floor
		// instead of sliding onto the neighbouring Door.
		{
			auto const plan = building.planResizeLift(liftSector, 6, 0, 1, 4);
			require(plan.valid, ("A shaft-extension resize was refused: " + plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 1, 2, 3 },
				"The shaft extension did not shift the stop offsets as expected");
			liftSector = building.applyLiftEdit(plan);
			require(stopFloor(building, liftSector, 0) == 1
				&& stopFloor(building, liftSector, 1) == 2
				&& stopFloor(building, liftSector, 2) == 3,
				"The shaft extension changed the stop floors");
			requireStyles(building, liftSector, styles, "Lift shaft extended below its stops");
		}

		// Widening the Lift while retaining the stops retains every style.
		{
			auto const plan = building.planResizeLift(liftSector, 6, 0, 2, 4);
			require(plan.valid, ("A width resize was refused: " + plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 1, 2, 3 },
				"The width change did not retain the stop offsets");
			liftSector = building.applyLiftEdit(plan);
			requireStyles(building, liftSector, styles, "Lift widened to two cells");
		}

		// The reconciled styles ride in the Lift's record: a save/load after
		// the move and resize retains them.
		auto const loaded = loadYaml(snapshotYaml(building));
		requireStyles(*loaded, liftSector, styles, "Loaded Lift after move and resize");

		// Deleting the middle stop takes its override with it; the surviving
		// stops keep their own styles rather than inheriting a neighbour's.
		{
			loaded->pauseSimulation();
			auto const plan = loaded->planRemoveLiftStop(liftSector, 1);
			require(plan.valid, ("Deleting the middle stop was refused: " + plan.diagnostic).c_str());
			auto const remaining = loaded->applyLiftEdit(plan);
			require(remaining == liftSector, "Removing a stop moved the Lift to another Sector");
			require(stopFloor(*loaded, liftSector, 0) == 1
				&& stopFloor(*loaded, liftSector, 1) == 3,
				"The remaining stops are not on the expected floors");
			requireStyles(*loaded, liftSector,
				{ core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenRight },
				"Lift after its middle stop was deleted");
		}
	}

	// Ticket #87: per-stop Door styles reconcile by stop identity when stops
	// are added, removed, or reordered.  A surviving stop keeps its own style,
	// a removed stop takes its override with it, a newly generated stop
	// receives the Lift's OpenApart default instead of any discarded style,
	// stop-list reindexing does not slide styles onto other stops, and the
	// reconciled result round-trips through save/load.
	void liftDoorStylesReconcileWhenStopsChange()
	{
		auto findLiftStopDoor = [](core::Building const& building, uint32_t liftSector,
			uint32_t stopIndex) -> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					if (building.isLiftOwnedDoor(object, &ownerSector, &ownerStop)
						&& ownerSector == liftSector && ownerStop == stopIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto stopFloor = [](core::Building const& building, uint32_t liftSector,
			uint32_t stopIndex) -> uint32_t
		{
			auto const lift = std::dynamic_pointer_cast<const core::LiftTransit>(
				building.getSector(liftSector));
			require(static_cast<bool>(lift), "The Lift transit disappeared");
			auto const& value = lift->getStop(stopIndex);
			return static_cast<uint32_t>((int)value.sector->getCellY() + value.sectorOffsetY);
		};
		auto stopCount = [](core::Building const& building, uint32_t liftSector) -> uint32_t
		{
			auto const lift = std::dynamic_pointer_cast<const core::LiftTransit>(
				building.getSector(liftSector));
			require(static_cast<bool>(lift), "The Lift transit disappeared");
			return lift->getNumStops();
		};
		// Styles are asserted against the stop's landing floor, not its index
		// in the stop list: the floor is the stop's identity for reconciliation.
		auto requireStyleAtFloor = [&](core::Building const& building, uint32_t liftSector,
			uint32_t floor, core::Door::OpenStyle expected, char const* context)
		{
			auto const count = stopCount(building, liftSector);
			for (uint32_t stop = 0; stop < count; ++stop)
			{
				if (stopFloor(building, liftSector, stop) != floor) continue;
				auto const door = findLiftStopDoor(building, liftSector, stop);
				require(static_cast<bool>(door),
					(std::string(context) + ": landing Door is missing at floor "
						+ std::to_string(floor)).c_str());
				require(door->getOpenStyle() == expected,
					(std::string(context) + ": the stop at floor " + std::to_string(floor)
						+ " does not carry its own style").c_str());
				return;
			}
			require(false, (std::string(context) + ": no stop exists at floor "
				+ std::to_string(floor)).c_str());
		};
		auto requireNoStopAtFloor = [](core::Building const& building, uint32_t liftSector,
			uint32_t floor, char const* context)
		{
			auto const lift = std::dynamic_pointer_cast<const core::LiftTransit>(
				building.getSector(liftSector));
			require(static_cast<bool>(lift), "The Lift transit disappeared");
			for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
			{
				auto const& value = lift->getStop(stop);
				auto const stopAt = static_cast<uint32_t>((int)value.sector->getCellY()
					+ value.sectorOffsetY);
				require(stopAt != floor,
					(std::string(context) + ": a stop still exists at floor "
						+ std::to_string(floor)).c_str());
			}
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};

		// Landings on floors 1-3; further landings are added mid-lifecycle so
		// stops can be created at the top and bottom of the shaft.
		core::Building building("Lift stop style reconciliation", 12, 5);
		building.addRoom("Landing 1", 0, 1, 0, 12, 1);
		building.addRoom("Landing 2", 0, 2, 0, 12, 1);
		building.addRoom("Landing 3", 0, 3, 0, 12, 1);
		auto const created = building.addLift(1, 1, 2, 1, 3);
		building.finishBuild();
		building.pauseSimulation();
		auto liftSector = created.lift.sector->getIndex();
		std::string diagnostic;
		require(created.doors.size() == 3, "The Lift did not generate three landing Doors");
		require(building.setLiftStopDoorOpenStyle(liftSector, 0,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Styling stop 0 was refused: " + diagnostic).c_str());
		require(building.setLiftStopDoorOpenStyle(liftSector, 1,
			core::Door::OpenStyle::OpenUp, &diagnostic),
			("Styling stop 1 was refused: " + diagnostic).c_str());
		require(building.setLiftStopDoorOpenStyle(liftSector, 2,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("Styling stop 2 was refused: " + diagnostic).c_str());
		requireStyleAtFloor(building, liftSector, 1, core::Door::OpenStyle::OpenLeft,
			"Freshly styled Lift");
		requireStyleAtFloor(building, liftSector, 2, core::Door::OpenStyle::OpenUp,
			"Freshly styled Lift");
		requireStyleAtFloor(building, liftSector, 3, core::Door::OpenStyle::OpenRight,
			"Freshly styled Lift");

		// Adding a stop: extending the shaft over a new landing creates a stop
		// at floor 4.  Every surviving stop keeps its own style and the new
		// Door takes the Lift's generated OpenApart default.  The added Room
		// record canonicalizes ahead of the Lift, so follow the Lift's Sector.
		{
			building.addRoom("Landing 4", 0, 4, 0, 12, 1);
			auto const plan = building.planResizeLift(liftSector, 2, 1, 1, 4);
			require(plan.valid, ("Extending the Lift over a new landing was refused: "
				+ plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2, 3 },
				"The extended Lift did not gain the new stop");
			liftSector = building.applyLiftEdit(plan);
			require(stopCount(building, liftSector) == 4,
				"The Lift did not gain exactly one new stop");
			requireStyleAtFloor(building, liftSector, 1, core::Door::OpenStyle::OpenLeft,
				"Lift after adding a stop");
			requireStyleAtFloor(building, liftSector, 2, core::Door::OpenStyle::OpenUp,
				"Lift after adding a stop");
			requireStyleAtFloor(building, liftSector, 3, core::Door::OpenStyle::OpenRight,
				"Lift after adding a stop");
			requireStyleAtFloor(building, liftSector, 4, core::Door::OpenStyle::OpenApart,
				"The newly added stop");
		}

		// Removing a stop: deleting the middle stop (floor 2) takes its OpenUp
		// override with it; the surviving stops keep their own styles.
		{
			auto const plan = building.planRemoveLiftStop(liftSector, 1);
			require(plan.valid, ("Removing the stop at floor 2 was refused: "
				+ plan.diagnostic).c_str());
			liftSector = building.applyLiftEdit(plan);
			require(stopCount(building, liftSector) == 3,
				"Removing a stop did not leave three stops");
			requireNoStopAtFloor(building, liftSector, 2,
				"Lift after removing its floor-2 stop");
			requireStyleAtFloor(building, liftSector, 1, core::Door::OpenStyle::OpenLeft,
				"Lift after removing a stop");
			requireStyleAtFloor(building, liftSector, 3, core::Door::OpenStyle::OpenRight,
				"Lift after removing a stop");
			requireStyleAtFloor(building, liftSector, 4, core::Door::OpenStyle::OpenApart,
				"Lift after removing a stop");
		}

		// A later new stop cannot inherit the discarded OpenUp override from
		// the removed stop it replaced at floor 2: the new identity is
		// generated with the Lift's OpenApart default while every survivor is
		// untouched.
		{
			auto const plan = building.planResizeLift(liftSector, 2, 1, 1, 4);
			require(plan.valid, ("Re-adding the stop at floor 2 was refused: "
				+ plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2, 3 },
				"The Lift did not re-derive the stop at floor 2");
			liftSector = building.applyLiftEdit(plan);
			require(stopCount(building, liftSector) == 4,
				"The Lift did not regain its fourth stop");
			requireStyleAtFloor(building, liftSector, 1, core::Door::OpenStyle::OpenLeft,
				"Lift after re-adding the floor-2 stop");
			requireStyleAtFloor(building, liftSector, 2, core::Door::OpenStyle::OpenApart,
				"The new stop replacing the removed one");
			requireStyleAtFloor(building, liftSector, 3, core::Door::OpenStyle::OpenRight,
				"Lift after re-adding the floor-2 stop");
			requireStyleAtFloor(building, liftSector, 4, core::Door::OpenStyle::OpenApart,
				"Lift after re-adding the floor-2 stop");
		}

		// Reordering stop data does not swap styles: adding a stop at the
		// bottom shifts every surviving stop to a new index in the stop list.
		// Each style follows its stop's landing floor instead of sliding onto
		// the Door that now sits at the style's old index.
		{
			building.addRoom("Landing 0", 0, 0, 0, 12, 1);
			auto const plan = building.planResizeLift(liftSector, 2, 0, 1, 5);
			require(plan.valid, ("Inserting a stop below the Lift was refused: "
				+ plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 1, 2, 3, 4 },
				"The Lift did not gain the bottom stop");
			liftSector = building.applyLiftEdit(plan);
			require(stopCount(building, liftSector) == 5,
				"Inserting the bottom stop changed the stop count unexpectedly");
			require(stopFloor(building, liftSector, 0) == 0
				&& stopFloor(building, liftSector, 1) == 1
				&& stopFloor(building, liftSector, 2) == 2
				&& stopFloor(building, liftSector, 3) == 3
				&& stopFloor(building, liftSector, 4) == 4,
				"The shifted stop list is not on the expected floors");
			requireStyleAtFloor(building, liftSector, 0, core::Door::OpenStyle::OpenApart,
				"The newly inserted bottom stop");
			requireStyleAtFloor(building, liftSector, 1, core::Door::OpenStyle::OpenLeft,
				"Lift after its stop indices shifted down");
			requireStyleAtFloor(building, liftSector, 2, core::Door::OpenStyle::OpenApart,
				"Lift after its stop indices shifted down");
			requireStyleAtFloor(building, liftSector, 3, core::Door::OpenStyle::OpenRight,
				"Lift after its stop indices shifted down");
			requireStyleAtFloor(building, liftSector, 4, core::Door::OpenStyle::OpenApart,
				"Lift after its stop indices shifted down");
		}

		// The reconciled result round-trips: a save/load after the adds,
		// the removal, and the reindexing retains every style on its own
		// stop's landing floor.
		{
			auto const yaml = snapshotYaml(building);
			auto const loaded = loadYaml(yaml);
			require(stopCount(*loaded, liftSector) == 5,
				"The loaded Lift lost stops in the round-trip");
			requireStyleAtFloor(*loaded, liftSector, 0, core::Door::OpenStyle::OpenApart,
				"Loaded Lift after stop reconciliation");
			requireStyleAtFloor(*loaded, liftSector, 1, core::Door::OpenStyle::OpenLeft,
				"Loaded Lift after stop reconciliation");
			requireStyleAtFloor(*loaded, liftSector, 2, core::Door::OpenStyle::OpenApart,
				"Loaded Lift after stop reconciliation");
			requireStyleAtFloor(*loaded, liftSector, 3, core::Door::OpenStyle::OpenRight,
				"Loaded Lift after stop reconciliation");
			requireStyleAtFloor(*loaded, liftSector, 4, core::Door::OpenStyle::OpenApart,
				"Loaded Lift after stop reconciliation");
		}
	}

	// Ticket #88: each generated Shuttle Door carries an individually authored
	// opening-style override while the Shuttle's topology stays fixed. Editing
	// one Shuttle Door changes no sibling Door at the same or another stop, the
	// overrides round-trip through Building persistence, snapshot-based
	// undo/redo restores the previous per-Door style, and reconstructing an
	// unchanged Shuttle retains every override. Shuttle Doors without an
	// override keep OpenUp.
	void shuttleDoorStyleOverridesAreIndividualAndPersist()
	{
		auto findShuttleDoor = [](core::Building const& building, uint32_t shuttleSector,
			uint32_t stopIndex, uint32_t carriageIndex, uint32_t doorIndex)
			-> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					uint32_t ownerCarriage{ ~0u }, ownerDoor{ ~0u };
					if (building.isShuttleOwnedDoor(object, &ownerSector, &ownerStop,
							&ownerCarriage, &ownerDoor)
						&& ownerSector == shuttleSector && ownerStop == stopIndex
						&& ownerCarriage == carriageIndex && ownerDoor == doorIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};

		core::Building building("Shuttle door styles", 32, 3);
		building.addCorridor(0, 0, 31);
		building.addCorridor(1, 0, 31);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101; // Two doors per carriage: cells 0 and 2.
		auto const created = building.addShuttle(1, 0, 0, 27, options);
		building.finishBuild();
		require(created.doors.size() == 8,
			"The Shuttle did not generate eight landing Doors");
		auto const shuttleSector = created.shuttle.sector->getIndex();
		building.pauseSimulation();

		// Every generated Shuttle Door without an override opens Up.
		for (uint32_t stop = 0; stop < 2; ++stop)
			for (uint32_t car = 0; car < 2; ++car)
				for (uint32_t door = 0; door < 2; ++door)
				{
					auto const generated = findShuttleDoor(building, shuttleSector, stop, car, door);
					require(generated != nullptr, "A generated Shuttle Door could not be found");
					require(generated->getOpenStyle() == core::Door::OpenStyle::OpenUp,
						"A Shuttle Door without an override is not OpenUp");
				}

		// A Shuttle with no per-Door choices persists no style data at all, and
		// overrides outside the authored stop/carriage/door grid are refused.
		require(snapshotYaml(building).find("doorOpenStyles") == std::string::npos,
			"A Shuttle with no per-Door overrides persisted a doorOpenStyles array");
		std::string diagnostic;
		require(!building.setShuttleDoorOpenStyle(shuttleSector, 2, 0, 0,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			"An override was accepted for a stop outside the Shuttle's authored topology");
		require(!building.setShuttleDoorOpenStyle(shuttleSector, 0, 2, 0,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			"An override was accepted for a carriage outside the Shuttle's authored topology");
		require(!building.setShuttleDoorOpenStyle(shuttleSector, 0, 0, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			"An override was accepted for a door cell outside the Shuttle's doorMask");

		// Editing one Door takes effect live and touches no sibling Door at the
		// same or another stop.
		require(building.setShuttleDoorOpenStyle(shuttleSector, 0, 0, 0,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("A per-Door override was refused: " + diagnostic).c_str());
		require(findShuttleDoor(building, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"The edited Shuttle Door did not take the override live");
		require(findShuttleDoor(building, shuttleSector, 0, 0, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Editing one Shuttle Door changed its sibling door on the same carriage");
		require(findShuttleDoor(building, shuttleSector, 0, 1, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Editing one Shuttle Door changed a sibling carriage at the same stop");
		require(findShuttleDoor(building, shuttleSector, 1, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Editing one Shuttle Door changed a sibling Door at another stop");

		// A second Door takes its own style while the first keeps its own.
		require(building.setShuttleDoorOpenStyle(shuttleSector, 1, 1, 1,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("A second per-Door override was refused: " + diagnostic).c_str());
		require(findShuttleDoor(building, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"Editing a second Shuttle Door changed the first Door's override");

		// Save/load: the overrides ride in the Shuttle's own record, and Doors
		// without an override replay as the generated OpenUp default.
		auto const yaml = snapshotYaml(building);
		require(yaml.find("doorOpenStyles") != std::string::npos,
			"The Shuttle record did not persist its per-Door styles");
		require(yaml.find("openLeft") != std::string::npos
			&& yaml.find("openRight") != std::string::npos
			&& yaml.find("default") != std::string::npos,
			"The persisted doorOpenStyles array lost a Door's style");
		auto loaded = loadYaml(yaml);
		require(findShuttleDoor(*loaded, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"A loaded Shuttle Door lost its OpenLeft override");
		require(findShuttleDoor(*loaded, shuttleSector, 1, 1, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"A loaded Shuttle Door lost its OpenRight override");
		require(findShuttleDoor(*loaded, shuttleSector, 1, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"A loaded Shuttle Door without an override lost the OpenUp default");

		// Undo/redo mirror: the editor snapshots YAML before and after a change
		// and restores either side verbatim.
		loaded->pauseSimulation();
		auto const before = snapshotYaml(*loaded);
		require(loaded->setShuttleDoorOpenStyle(shuttleSector, 0, 0, 0,
			core::Door::OpenStyle::OpenApart, &diagnostic),
			("A re-author to OpenApart was refused: " + diagnostic).c_str());
		auto const after = snapshotYaml(*loaded);
		require(findShuttleDoor(*loaded, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenApart,
			"The re-authored Shuttle Door did not take OpenApart live");
		require(findShuttleDoor(*loaded, shuttleSector, 1, 1, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"The re-author disturbed a sibling Door");
		auto const undone = loadYaml(before);
		require(findShuttleDoor(*undone, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenLeft,
			"Undo did not restore the previous per-Door style");
		require(findShuttleDoor(*undone, shuttleSector, 1, 1, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"Undo disturbed another Door's override");
		auto const redone = loadYaml(after);
		require(findShuttleDoor(*redone, shuttleSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenApart,
			"Redo did not restore the edited per-Door style");

		// Reconstructing the Shuttle with unchanged topology retains every
		// override: the edit plan rewrites the record in place and the overrides
		// stay attached to their grid slots.
		redone->pauseSimulation();
		auto const rebuildPlan = redone->planResizeShuttle(shuttleSector, 0, 0, 27);
		require(rebuildPlan.valid,
			("An unchanged-topology Shuttle rebuild plan was refused: "
				+ rebuildPlan.diagnostic).c_str());
		require(rebuildPlan.stopOffsets == std::vector<uint32_t>{ 0, 18 },
			"The rebuild plan did not keep the Shuttle's stop topology");
		auto const rebuiltSector = redone->applyShuttleEdit(rebuildPlan);
		require(rebuiltSector == shuttleSector, "The rebuilt Shuttle moved to another Sector");
		require(findShuttleDoor(*redone, rebuiltSector, 0, 0, 0)->getOpenStyle()
			== core::Door::OpenStyle::OpenApart,
			"Reconstructing an unchanged Shuttle lost an edited override");
		require(findShuttleDoor(*redone, rebuiltSector, 1, 1, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenRight,
			"Reconstructing an unchanged Shuttle lost a second override");
		require(findShuttleDoor(*redone, rebuiltSector, 0, 1, 1)->getOpenStyle()
			== core::Door::OpenStyle::OpenUp,
			"Reconstructing an unchanged Shuttle changed a Door without an override");
	}

	// Ticket #89: moving a Shuttle with unchanged stops, carriages, and door
	// positions preserves every per-Door style.  Styles follow the structural
	// Door identity - owning Shuttle, stop, carriage, configured door position -
	// rather than transient object indices: a sideways or vertical move, a
	// restyle at the new position, a second move, a topology-equivalent
	// reconstruction, and a save/load all keep every style on its own Door.
	void shuttleDoorStylesSurviveShuttleMovement()
	{
		auto findShuttleDoor = [](core::Building const& building, uint32_t shuttleSector,
			uint32_t stopIndex, uint32_t carriageIndex, uint32_t doorIndex)
			-> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					uint32_t ownerCarriage{ ~0u }, ownerDoor{ ~0u };
					if (building.isShuttleOwnedDoor(object, &ownerSector, &ownerStop,
							&ownerCarriage, &ownerDoor)
						&& ownerSector == shuttleSector && ownerStop == stopIndex
						&& ownerCarriage == carriageIndex && ownerDoor == doorIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};
		// Every one of the eight stop/carriage/door identities carries its own
		// expected style; no two adjacent identities share a style so a swap
		// between any pair of siblings is visible.
		core::Door::OpenStyle expected[2][2][2] = {
			{ { core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenApart },
			  { core::Door::OpenStyle::OpenRight, core::Door::OpenStyle::OpenUp } },
			{ { core::Door::OpenStyle::OpenApart, core::Door::OpenStyle::OpenRight },
			  { core::Door::OpenStyle::OpenUp, core::Door::OpenStyle::OpenLeft } } };
		auto requireAllStyles = [&](core::Building const& building, uint32_t shuttleSector,
			char const* context)
		{
			for (uint32_t stop = 0; stop < 2; ++stop)
				for (uint32_t car = 0; car < 2; ++car)
					for (uint32_t door = 0; door < 2; ++door)
					{
						auto const found = findShuttleDoor(building, shuttleSector, stop, car, door);
						require(found != nullptr,
							("Shuttle Door identity lost in " + std::string(context)).c_str());
						require(found->getOpenStyle() == expected[stop][car][door],
							("Shuttle Door style wrong in " + std::string(context)).c_str());
					}
		};
		auto requireTransitAt = [](core::Building const& building, uint32_t shuttleSector,
			uint32_t x, uint32_t y, char const* context)
		{
			auto const transit = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				building.getSector(shuttleSector));
			require(transit != nullptr,
				("The Shuttle Sector is not a Shuttle Transit in " + std::string(context)).c_str());
			require(transit->getCellX() == x && transit->getCellY() == y,
				("The Shuttle did not reach its requested position in " + std::string(context)).c_str());
		};

		core::Building building("Shuttle move styles", 48, 3);
		for (uint32_t row = 0; row < 3; ++row)
			building.addCorridor(0u, row, 0u, 47u, 1u);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101; // Two doors per carriage: cells 0 and 2.
		auto const created = building.addShuttle(1, 0, 0, 27, options);
		building.finishBuild();
		building.pauseSimulation();
		auto shuttleSector = created.shuttle.sector->getIndex();

		std::string diagnostic;
		for (uint32_t stop = 0; stop < 2; ++stop)
			for (uint32_t car = 0; car < 2; ++car)
				for (uint32_t door = 0; door < 2; ++door)
					require(building.setShuttleDoorOpenStyle(shuttleSector, stop, car, door,
						expected[stop][car][door], &diagnostic),
						("Styling a Shuttle Door was refused: " + diagnostic).c_str());
		requireAllStyles(building, shuttleSector, "authored styles");

		// Move the Shuttle sideways without changing stops, carriages, or door
		// positions.  Every style must follow its own structural Door identity.
		{
			auto const plan = building.planResizeShuttle(shuttleSector, 4, 0, 27);
			require(plan.valid, ("Moving the Shuttle was refused: " + plan.diagnostic).c_str());
			require(plan.move, "A sideways Shuttle edit was not recognized as a move");
			require(plan.stopOffsets == std::vector<uint32_t>{ 0, 18 },
				"The move changed the Shuttle's stop topology unexpectedly");
			shuttleSector = building.applyShuttleEdit(plan);
			requireTransitAt(building, shuttleSector, 4, 0, "Shuttle after moving");
			requireAllStyles(building, shuttleSector, "Shuttle after moving");
		}

		// Restyling at the new position addresses the same structural identity:
		// the live Door the panel finds after the move is the Door the override
		// grid slot governs, not a transient neighbour.
		{
			expected[1][1][1] = core::Door::OpenStyle::OpenRight;
			expected[0][0][0] = core::Door::OpenStyle::OpenUp;
			require(building.setShuttleDoorOpenStyle(shuttleSector, 1, 1, 1,
				core::Door::OpenStyle::OpenRight, &diagnostic),
				("Restyling after the move was refused: " + diagnostic).c_str());
			require(building.setShuttleDoorOpenStyle(shuttleSector, 0, 0, 0,
				core::Door::OpenStyle::OpenUp, &diagnostic),
				("Restyling after the move was refused: " + diagnostic).c_str());
			requireAllStyles(building, shuttleSector, "Shuttle restyled after moving");
		}

		// A second move, this time vertically as well as sideways, keeps every
		// style on its own stop/carriage/door identity.
		{
			auto const plan = building.planResizeShuttle(shuttleSector, 10, 1, 27);
			require(plan.valid, ("Moving the Shuttle to another row was refused: "
				+ plan.diagnostic).c_str());
			require(plan.move, "A diagonal Shuttle edit was not recognized as a move");
			shuttleSector = building.applyShuttleEdit(plan);
			requireTransitAt(building, shuttleSector, 10, 1, "Shuttle after the second move");
			requireAllStyles(building, shuttleSector, "Shuttle after the second move");
		}

		// Extending the track to the left shifts every stop offset while keeping
		// the same stops, carriages, and door configuration; styles must stay on
		// their own structural identities, not slide with the offsets.
		{
			auto const plan = building.planResizeShuttle(shuttleSector, 8, 1, 29);
			require(plan.valid, ("Extending the Shuttle track leftward was refused: "
				+ plan.diagnostic).c_str());
			require(plan.stopOffsets == std::vector<uint32_t>{ 2, 20 },
				("Left extension did not shift stop offsets as expected: got "
					+ std::to_string(plan.stopOffsets.size()) + " stops").c_str());
			shuttleSector = building.applyShuttleEdit(plan);
			requireTransitAt(building, shuttleSector, 8, 1, "Shuttle after left extension");
			requireAllStyles(building, shuttleSector, "Shuttle after left extension");
		}

		// A topology-equivalent reconstruction at the new position must not
		// reset or swap styles.
		{
			auto const plan = building.planResizeShuttle(shuttleSector, 8, 1, 29);
			require(plan.valid, ("Reconstructing the moved Shuttle was refused: "
				+ plan.diagnostic).c_str());
			shuttleSector = building.applyShuttleEdit(plan);
			requireTransitAt(building, shuttleSector, 8, 1,
				"Shuttle after topology-equivalent reconstruction");
			requireAllStyles(building, shuttleSector,
				"Shuttle after topology-equivalent reconstruction");
		}

		// Save/load after the moves retains the reconciled styles.
		{
			auto const loaded = loadYaml(snapshotYaml(building));
			requireTransitAt(*loaded, shuttleSector, 8, 1, "Loaded Shuttle after movement");
			requireAllStyles(*loaded, shuttleSector, "Loaded Shuttle after movement");
		}
	}

	// Ticket #90: Shuttle Door styles reconcile when the stop set or partial
	// landing support changes.  Surviving stop/carriage/door identities retain
	// their authored styles across stop removal, stop addition, and the index
	// shifts they cause; a deleted stop and an omitted partial-landing Door
	// take their overrides with them instead of leaking a style onto a
	// different stop, carriage, or door position; newly added or newly
	// supported Doors generate OpenUp; and the reconciled result round-trips
	// through save/load.
	void shuttleDoorStylesReconcileWhenStopsChange()
	{
		auto findShuttleDoor = [](core::Building const& building, uint32_t shuttleSector,
			uint32_t stopIndex, uint32_t carriageIndex, uint32_t doorIndex)
			-> std::shared_ptr<const core::Door>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					uint32_t ownerCarriage{ ~0u }, ownerDoor{ ~0u };
					if (building.isShuttleOwnedDoor(object, &ownerSector, &ownerStop,
							&ownerCarriage, &ownerDoor)
						&& ownerSector == shuttleSector && ownerStop == stopIndex
						&& ownerCarriage == carriageIndex && ownerDoor == doorIndex)
						return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				}
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};
		// The style the Shuttle's own record carries for one grid slot,
		// ~0u when the record holds no override there.
		auto recordedStyle = [](core::Building const& building, uint32_t shuttleSector,
			uint32_t stop, uint32_t car, uint32_t door) -> uint32_t
		{
			auto const transit = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				building.getSector(shuttleSector));
			require(transit != nullptr, "The Shuttle Sector is not a Shuttle Transit");
			core::Building::CreateShuttleOptions options{};
			require(building.getShuttleOptions(transit->getShuttle().get(), options),
				"The Shuttle record could not be read back");
			auto const slot = (stop * 2 + car) * 2 + door;
			return slot < options.doorOpenStyles.size() ? options.doorOpenStyles[slot] : ~0u;
		};
		// Every live Shuttle-owned Door column, to prove removed stops take
		// their Doors with them and omitted partial landings stay unbuilt.
		auto shuttleDoorColumns = [](core::Building const& building, uint32_t shuttleSector)
		{
			std::set<uint32_t> columns;
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					uint32_t ownerSector{ ~0u };
					if (building.isShuttleOwnedDoor(object, &ownerSector)
						&& ownerSector == shuttleSector)
						columns.insert(object->getCellX());
				}
			}
			return columns;
		};
		using Style = core::Door::OpenStyle;
		// present[stop][car][door]: the Door must exist and carry the
		// expected style (when present) or must not exist at all.
		auto requireGrid = [&](core::Building const& building, uint32_t shuttleSector,
			bool const present[3][2][2], Style const expected[3][2][2], char const* context)
		{
			for (uint32_t stop = 0; stop < 3; ++stop)
				for (uint32_t car = 0; car < 2; ++car)
					for (uint32_t door = 0; door < 2; ++door)
					{
						auto const found = findShuttleDoor(building, shuttleSector, stop, car, door);
						if (!present[stop][car][door])
						{
							require(found == nullptr,
								("A Door that should be absent exists in " + std::string(context)).c_str());
							continue;
						}
						require(found != nullptr,
							("A surviving Shuttle Door was lost in " + std::string(context)).c_str());
						require(found->getOpenStyle() == expected[stop][car][door],
							("A Shuttle Door style is wrong in " + std::string(context)).c_str());
					}
		};
		auto copyPresence = [](bool dst[2][2], bool const src[2][2])
		{
			for (uint32_t car = 0; car < 2; ++car)
				for (uint32_t door = 0; door < 2; ++door) dst[car][door] = src[car][door];
		};
		auto copyStyles = [](Style dst[2][2], Style const src[2][2])
		{
			for (uint32_t car = 0; car < 2; ++car)
				for (uint32_t door = 0; door < 2; ++door) dst[car][door] = src[car][door];
		};

		// The front-layer platform leaves cells 31 and 32 empty: the last
		// stop's first carriage second door has no landing and is omitted as
		// an unsupported partial landing.
		core::Building building("Shuttle stop style reconciliation", 48, 3);
		building.addCorridor(0, 0, 0, 31, 1);
		building.addCorridor(0, 0, 33, 15, 1);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 18, 29 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101; // Two doors per carriage: cells 0 and 2.
		options.allowPartialLandings = true;
		auto const created = building.addShuttle(1, 0, 0, 40, options);
		building.finishBuild();
		require(created.doors.size() == 12,
			"The Shuttle did not generate its twelve-slot landing Door grid");
		require(!created.doors[(2 * 2 + 0) * 2 + 1].traversalResource,
			"An unsupported partial landing produced a Door instead of being omitted");
		building.pauseSimulation();
		auto shuttleSector = created.shuttle.sector->getIndex();
		require(shuttleDoorColumns(building, shuttleSector)
			== std::set<uint32_t>{ 0, 2, 4, 6, 18, 20, 22, 24, 29, 33, 35 },
			"The authored Shuttle does not own the expected landing Door columns");

		// Style rows are tracked by stop identity, not by stop index:
		// rowA is the stop at global X 0, rowB the stop at global X 18, and
		// rowC the stop at global X 29 whose one partial landing is omitted.
		Style const rowA[2][2] = { { Style::OpenLeft, Style::OpenApart },
			{ Style::OpenRight, Style::OpenUp } };
		Style const rowB[2][2] = { { Style::OpenRight, Style::OpenLeft },
			{ Style::OpenUp, Style::OpenApart } };
		Style const rowC[2][2] = { { Style::OpenApart, Style::OpenUp },
			{ Style::OpenLeft, Style::OpenRight } };
		Style const rowNew[2][2] = { { Style::OpenUp, Style::OpenUp },
			{ Style::OpenUp, Style::OpenUp } };
		bool const fullStop[2][2] = { { true, true }, { true, true } };
		bool const partialStop[2][2] = { { true, false }, { true, true } };
		bool const noStop[2][2] = { { false, false }, { false, false } };
		bool present[3][2][2];
		Style expected[3][2][2];
		copyPresence(present[0], fullStop);
		copyPresence(present[1], fullStop);
		copyPresence(present[2], partialStop);
		copyStyles(expected[0], rowA);
		copyStyles(expected[1], rowB);
		copyStyles(expected[2], rowC);

		std::string diagnostic;
		for (uint32_t stop = 0; stop < 3; ++stop)
			for (uint32_t car = 0; car < 2; ++car)
				for (uint32_t door = 0; door < 2; ++door)
				{
					if (!present[stop][car][door]) continue;
					require(building.setShuttleDoorOpenStyle(shuttleSector, stop, car, door,
						expected[stop][car][door], &diagnostic),
						("Styling a Shuttle Door was refused: " + diagnostic).c_str());
				}
		// An override addressed to the omitted partial-landing Door is a
		// valid grid slot that governs no live Door; reconciliation must
		// take it away with the omission rather than leave it live.
		require(building.setShuttleDoorOpenStyle(shuttleSector, 2, 0, 1,
			Style::OpenRight, &diagnostic),
			("Styling the omitted partial-landing slot was refused: " + diagnostic).c_str());
		require(findShuttleDoor(building, shuttleSector, 2, 0, 1) == nullptr,
			"The omitted partial-landing slot has a live Door");
		requireGrid(building, shuttleSector, present, expected, "authored styles");

		// Deleting the middle stop reindexes the far stop: its styles stay on
		// its own stop, the deleted stop's overrides leave with it, and the
		// override on the omitted partial-landing Door is dropped too.
		{
			auto const plan = building.planRemoveShuttleStop(shuttleSector, 1);
			require(plan.valid, ("Deleting the middle Shuttle stop was refused: " + plan.diagnostic).c_str());
			shuttleSector = building.applyShuttleEdit(plan);
			copyPresence(present[0], fullStop);
			copyPresence(present[1], partialStop);
			copyPresence(present[2], noStop);
			copyStyles(expected[0], rowA);
			copyStyles(expected[1], rowC);
			requireGrid(building, shuttleSector, present, expected, "after deleting the middle stop");
			require(shuttleDoorColumns(building, shuttleSector)
				== std::set<uint32_t>{ 0, 2, 4, 6, 29, 33, 35 },
				"The deleted stop left Doors behind or the omitted landing gained one");
			require(recordedStyle(building, shuttleSector, 1, 0, 1) == ~0u,
				"The omitted partial-landing Door retained a live override");
		}

		// Adding a stop in the middle shifts the indices again: every
		// survivor keeps its own style and the new stop's Doors generate
		// OpenUp with no override of their own.
		{
			auto const plan = building.planAddShuttleStop(shuttleSector, 10);
			require(plan.valid, ("Adding a Shuttle stop was refused: " + plan.diagnostic).c_str());
			shuttleSector = building.applyShuttleEdit(plan);
			copyPresence(present[0], fullStop);
			copyPresence(present[1], fullStop);
			copyPresence(present[2], partialStop);
			copyStyles(expected[0], rowA);
			copyStyles(expected[1], rowNew);
			copyStyles(expected[2], rowC);
			requireGrid(building, shuttleSector, present, expected, "after adding a middle stop");
			require(recordedStyle(building, shuttleSector, 1, 0, 0) == ~0u
				&& recordedStyle(building, shuttleSector, 1, 1, 1) == ~0u,
				"A newly added stop's Door did not default to no override");
			require(recordedStyle(building, shuttleSector, 2, 0, 1) == ~0u,
				"The omitted partial-landing override survived the stop addition");
		}

		// A styled stop that is deleted and later re-added at the same
		// offset cannot resurrect its discarded overrides: the re-added
		// Doors are new identities and generate OpenUp.
		{
			auto const remove = building.planRemoveShuttleStop(shuttleSector, 0);
			require(remove.valid, ("Deleting the styled first stop was refused: " + remove.diagnostic).c_str());
			shuttleSector = building.applyShuttleEdit(remove);
			auto const readd = building.planAddShuttleStop(shuttleSector, 0);
			require(readd.valid, ("Re-adding the deleted stop offset was refused: " + readd.diagnostic).c_str());
			shuttleSector = building.applyShuttleEdit(readd);
			copyPresence(present[0], fullStop);
			copyPresence(present[1], fullStop);
			copyPresence(present[2], partialStop);
			copyStyles(expected[0], rowNew);
			copyStyles(expected[1], rowNew);
			copyStyles(expected[2], rowC);
			requireGrid(building, shuttleSector, present, expected,
				"after re-adding the deleted first stop");
			require(recordedStyle(building, shuttleSector, 0, 0, 0) == ~0u
				&& recordedStyle(building, shuttleSector, 0, 1, 1) == ~0u,
				"A re-added stop resurrected the deleted stop's overrides");
		}

		// Partial-landing support changes: a new Corridor fills the gap at
		// cells 31-32, so the omitted Door becomes supported.  The next
		// Shuttle rebuild builds it fresh at OpenUp - the override dropped
		// with the omission does not come back.
		{
			building.addCorridor(0, 0, 31, 2, 1);
			auto const plan = building.planResizeShuttle(shuttleSector, 0, 0, 40);
			require(plan.valid, ("Rebuilding the Shuttle over the extended platform was refused: "
				+ plan.diagnostic).c_str());
			shuttleSector = building.applyShuttleEdit(plan);
			copyPresence(present[0], fullStop);
			copyPresence(present[1], fullStop);
			copyPresence(present[2], fullStop);
			copyStyles(expected[0], rowNew);
			copyStyles(expected[1], rowNew);
			copyStyles(expected[2], rowC);
			expected[2][0][1] = Style::OpenUp;
			requireGrid(building, shuttleSector, present, expected,
				"after the omitted landing became supported");
			require(shuttleDoorColumns(building, shuttleSector).count(31) == 1,
				"The newly supported landing did not gain its Door");
			require(recordedStyle(building, shuttleSector, 2, 0, 1) == ~0u,
				"The newly supported Door carried a stale override");
		}

		// The reconciled result round-trips through save/load unchanged.
		{
			auto const loaded = loadYaml(snapshotYaml(building));
			requireGrid(*loaded, shuttleSector, present, expected, "loaded Shuttle");
			require(recordedStyle(*loaded, shuttleSector, 2, 0, 1) == ~0u,
				"The loaded Shuttle carried an override for the once-omitted Door");
			require(recordedStyle(*loaded, shuttleSector, 0, 0, 0) == ~0u,
				"The loaded Shuttle carried a stale override on the re-added stop");
			require(recordedStyle(*loaded, shuttleSector, 2, 1, 1)
				== static_cast<uint32_t>(Style::OpenRight),
				"The loaded Shuttle lost a surviving stop's override");
		}
	}

	// Ticket #91: Shuttle Door styles reconcile when the coupled vehicle changes.
	// A style survives only where its own stop, carriage index, and configured
	// carriage cell still exist and still land on a supported landing.  Adding or
	// removing carriages keeps every surviving carriage's styles and takes the
	// removed carriage's overrides with it; deselecting a carriage door position
	// discards that Door's style instead of letting the compacted door index
	// slide it onto the next cell, and reselecting the cell later does not
	// resurrect it; a carriage width change moves each style to the physical
	// Door its identity now addresses - never to a neighbour - and a Door that
	// the widening moves onto no landing loses its override rather than staying
	// live.  Every newly generated Door uses OpenUp, and the reconciled result
	// round-trips through save/load.
	void shuttleDoorStylesReconcileWhenCarriageAndDoorLayoutChanges()
	{
		auto findShuttleDoorObject = [](core::Building const& building, uint32_t shuttleSector,
			uint32_t stopIndex, uint32_t carriageIndex, uint32_t doorIndex)
			-> std::shared_ptr<const core::SectorObject>
		{
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, ownerStop{ ~0u };
					uint32_t ownerCarriage{ ~0u }, ownerDoor{ ~0u };
					if (building.isShuttleOwnedDoor(object, &ownerSector, &ownerStop,
							&ownerCarriage, &ownerDoor)
						&& ownerSector == shuttleSector && ownerStop == stopIndex
						&& ownerCarriage == carriageIndex && ownerDoor == doorIndex)
						return object;
				}
			}
			return nullptr;
		};
		auto findShuttleDoor = [&findShuttleDoorObject](core::Building const& building,
			uint32_t shuttleSector, uint32_t stopIndex, uint32_t carriageIndex, uint32_t doorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto const object = findShuttleDoorObject(building, shuttleSector, stopIndex,
				carriageIndex, doorIndex);
			return object ? static_pointer_cast<const core::DoorSectorObject>(object)->getDoor()
				: nullptr;
		};
		auto snapshotYaml = [](core::Building& building)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			building.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto building = std::make_shared<core::Building>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			building->deserialize(*reader, workData);
			return building;
		};
		auto offsetsOf = [](uint32_t mask)
		{
			std::vector<uint32_t> offsets;
			for (uint32_t cell = 0; cell < 32; ++cell)
				if ((mask & (1u << cell)) != 0) offsets.push_back(cell);
			return offsets;
		};
		// The style the Shuttle's own record carries for one grid slot,
		// ~0u when the record holds no override there.
		auto recordedStyle = [](core::Building const& building, uint32_t shuttleSector,
			uint32_t numCars, uint32_t doorCount, uint32_t stop, uint32_t car, uint32_t door)
			-> uint32_t
		{
			auto const transit = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				building.getSector(shuttleSector));
			require(transit != nullptr, "The Shuttle Sector is not a Shuttle Transit");
			core::Building::CreateShuttleOptions options{};
			require(building.getShuttleOptions(transit->getShuttle().get(), options),
				"The Shuttle record could not be read back");
			auto const slot = (stop * numCars + car) * doorCount + door;
			return slot < options.doorOpenStyles.size() ? options.doorOpenStyles[slot] : ~0u;
		};
		auto shuttleDoorColumns = [](core::Building const& building, uint32_t shuttleSector)
		{
			std::set<uint32_t> columns;
			for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
			{
				auto const sector = building.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					uint32_t ownerSector{ ~0u };
					if (building.isShuttleOwnedDoor(object, &ownerSector)
						&& ownerSector == shuttleSector)
						columns.insert(object->getCellX());
				}
			}
			return columns;
		};
		using Style = core::Door::OpenStyle;
		// The front Layer leaves cell 7 of row 0 empty: a Door whose cell moves
		// there has no landing, so it is never built and keeps no override.
		uint32_t const holeX = 7;
		// expected holds the authored styles that should still be live, keyed by
		// (stop, carriage, configured carriage cell).  Any identity absent from
		// the map must show the generated OpenUp default.
		auto requireLayout = [&](core::Building const& building, uint32_t shuttleSector,
			uint32_t shuttleX, uint32_t numCars, uint32_t carWidth, uint32_t doorMask,
			std::vector<uint32_t> const& stopOffsets,
			std::map<std::array<uint32_t, 3>, Style> const& expected,
			std::set<uint32_t> const& expectedColumns, char const* context)
		{
			auto const offsets = offsetsOf(doorMask);
			auto const doorCount = static_cast<uint32_t>(offsets.size());
			auto const transit = std::dynamic_pointer_cast<const core::ShuttleTransit>(
				building.getSector(shuttleSector));
			require(transit != nullptr, "The Shuttle Sector is not a Shuttle Transit");
			core::Building::CreateShuttleOptions options{};
			require(building.getShuttleOptions(transit->getShuttle().get(), options),
				"The Shuttle record could not be read back");
			require(options.numCars == numCars && options.carWidth == carWidth
				&& options.doorMask == doorMask,
				("The Shuttle vehicle was not re-authored as planned in "
					+ std::string(context)).c_str());
			for (uint32_t stop = 0; stop < stopOffsets.size(); ++stop)
				for (uint32_t car = 0; car < numCars; ++car)
					for (uint32_t door = 0; door < doorCount; ++door)
					{
						auto const offset = offsets[door];
						auto const doorX = shuttleX + stopOffsets[stop]
							+ car * (carWidth + 1) + offset;
						std::array<uint32_t, 3> const key{ stop, car, offset };
						auto const authored = expected.find(key);
						auto const label = std::string(context) + " (stop " + std::to_string(stop)
							+ ", carriage " + std::to_string(car) + ", carriage cell "
							+ std::to_string(offset) + ")";
						auto const recorded = recordedStyle(building, shuttleSector, numCars,
								doorCount, stop, car, door);
						if (doorX == holeX)
						{
							require(findShuttleDoor(building, shuttleSector, stop, car, door) == nullptr,
								("A Door was built on a landing that does not exist in " + label).c_str());
							require(recorded == ~0u,
								("An override survived a Door with no landing in " + label).c_str());
							continue;
						}
						auto const found = findShuttleDoor(building, shuttleSector, stop, car, door);
						require(found != nullptr,
							("A supported Shuttle Door is missing in " + label).c_str());
						auto const foundObject = findShuttleDoorObject(building, shuttleSector,
							stop, car, door);
						require(foundObject != nullptr && foundObject->getCellX() == doorX,
							("A Shuttle Door does not sit on the cell its own identity addresses in "
								+ label).c_str());
						auto const want = authored == expected.end() ? Style::OpenUp : authored->second;
						require(found->getOpenStyle() == want,
							("A Shuttle Door style is wrong in " + label).c_str());
						auto const wantRecord = authored == expected.end()
							? ~0u : static_cast<uint32_t>(authored->second);
						require(recorded == wantRecord,
							("The recorded override disagrees with the live Door in " + label).c_str());
					}
			require(shuttleDoorColumns(building, shuttleSector) == expectedColumns,
				("The Shuttle does not own the expected landing Door columns in "
					+ std::string(context)).c_str());
		};
		auto applyVehicle = [](core::Building& building, uint32_t shuttleSector,
			uint32_t numCars, uint32_t carWidth, uint32_t doorMask) -> uint32_t
		{
			auto const plan = building.planEditShuttleVehicle(shuttleSector, numCars, carWidth, doorMask);
			require(plan.valid, ("Re-authoring the Shuttle vehicle was refused: " + plan.diagnostic).c_str());
			require(!plan.move, "A vehicle-only edit was mistaken for a Shuttle move");
			require(plan.requiresConfirmation(),
				"A vehicle change reported no consequence for the rebuilt landings");
			require(plan.numCars == numCars && plan.carWidth == carWidth && plan.doorMask == doorMask,
				"The plan did not carry the requested vehicle layout");
			return building.applyShuttleEdit(plan);
		};

		// The front Layer covers every landing column the Shuttle will ever use
		// except cell 7, which stays empty for the width-change step.
		core::Building building("Shuttle vehicle style reconciliation", 64, 3);
		building.addCorridor(0, 0, 0, 7, 1);
		building.addCorridor(0, 0, 8, 44, 1);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 20 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101; // Two doors per carriage: cells 0 and 2.
		options.allowPartialLandings = true;
		auto const created = building.addShuttle(1, 0, 0, 40, options);
		building.finishBuild();
		building.pauseSimulation();
		auto shuttleSector = created.shuttle.sector->getIndex();
		std::vector<uint32_t> const stops{ 0, 20 };

		// Style all eight authored Doors, no two neighbours alike, so any leak
		// from one identity to another is visible.
		std::map<std::array<uint32_t, 3>, Style> expected{
			{ { 0, 0, 0 }, Style::OpenLeft }, { { 0, 0, 2 }, Style::OpenApart },
			{ { 0, 1, 0 }, Style::OpenRight }, { { 0, 1, 2 }, Style::OpenLeft },
			{ { 1, 0, 0 }, Style::OpenApart }, { { 1, 0, 2 }, Style::OpenRight },
			{ { 1, 1, 0 }, Style::OpenLeft }, { { 1, 1, 2 }, Style::OpenApart } };
		std::string diagnostic;
		struct Slot { uint32_t stop; uint32_t car; uint32_t offset; Style style; };
		auto const authoredOffsets = offsetsOf(0b101);
		for (auto const& entry : expected)
		{
			auto const& key = entry.first;
			auto const doorIndex = static_cast<uint32_t>(
				std::find(authoredOffsets.begin(), authoredOffsets.end(), key[2])
				- authoredOffsets.begin());
			require(building.setShuttleDoorOpenStyle(shuttleSector, key[0], key[1], doorIndex,
				entry.second, &diagnostic),
				("Styling an authored Shuttle Door was refused: " + diagnostic).c_str());
		}
		requireLayout(building, shuttleSector, 0, 2, 3, 0b101, stops, expected,
			{ 0, 2, 4, 6, 20, 22, 24, 26 }, "authored two-carriage Shuttle");

		// Adding a carriage keeps every surviving carriage's styles and gives the
		// new carriage the OpenUp default.
		shuttleSector = applyVehicle(building, shuttleSector, 3, 3, 0b101);
		requireLayout(building, shuttleSector, 0, 3, 3, 0b101, stops, expected,
			{ 0, 2, 4, 6, 8, 10, 20, 22, 24, 26, 28, 30 },
			"Shuttle with an added carriage");

		// Style the new carriage so its removal has something to take away.
		for (auto const& slot : std::vector<Slot>{
			{ 0, 2, 0, Style::OpenRight }, { 0, 2, 2, Style::OpenLeft },
			{ 1, 2, 0, Style::OpenApart }, { 1, 2, 2, Style::OpenRight } })
		{
			auto const doorIndex = slot.offset == 0u ? 0u : 1u;
			require(building.setShuttleDoorOpenStyle(shuttleSector, slot.stop, slot.car, doorIndex,
				slot.style, &diagnostic),
				("Styling the added carriage's Door was refused: " + diagnostic).c_str());
			expected[std::array<uint32_t, 3>{ slot.stop, slot.car, slot.offset }] = slot.style;
		}
		requireLayout(building, shuttleSector, 0, 3, 3, 0b101, stops, expected,
			{ 0, 2, 4, 6, 8, 10, 20, 22, 24, 26, 28, 30 },
			"Shuttle with the added carriage styled");

		// Dropping the carriage takes its overrides with it: the surviving
		// carriages keep their own styles and nothing shifts sideways.
		expected = {
			{ { 0, 0, 0 }, Style::OpenLeft }, { { 0, 0, 2 }, Style::OpenApart },
			{ { 0, 1, 0 }, Style::OpenRight }, { { 0, 1, 2 }, Style::OpenLeft },
			{ { 1, 0, 0 }, Style::OpenApart }, { { 1, 0, 2 }, Style::OpenRight },
			{ { 1, 1, 0 }, Style::OpenLeft }, { { 1, 1, 2 }, Style::OpenApart } };
		shuttleSector = applyVehicle(building, shuttleSector, 2, 3, 0b101);
		requireLayout(building, shuttleSector, 0, 2, 3, 0b101, stops, expected,
			{ 0, 2, 4, 6, 20, 22, 24, 26 }, "Shuttle with the carriage removed");
		for (uint32_t stop = 0; stop < 2; ++stop)
			for (uint32_t door = 0; door < 2; ++door)
				require(findShuttleDoor(building, shuttleSector, stop, 2, door) == nullptr,
					"The removed carriage still owns a landing Door");

		// Deselecting carriage cell 0 discards those Doors' styles instead of
		// letting the compacted door index slide them onto cell 1, and cell 1 is
		// newly configured so it generates OpenUp.
		expected = {
			{ { 0, 0, 2 }, Style::OpenApart }, { { 0, 1, 2 }, Style::OpenLeft },
			{ { 1, 0, 2 }, Style::OpenRight }, { { 1, 1, 2 }, Style::OpenApart } };
		shuttleSector = applyVehicle(building, shuttleSector, 2, 3, 0b110);
		requireLayout(building, shuttleSector, 0, 2, 3, 0b110, stops, expected,
			{ 1, 2, 5, 6, 21, 22, 25, 26 }, "Shuttle with cell 0 deselected");
		require(findShuttleDoor(building, shuttleSector, 0, 0, 0)->getOpenStyle()
			== Style::OpenUp,
			"The deselected cell 0 style slid onto the newly configured cell 1");

		// Re-selecting cell 0 does not resurrect the style it carried before it
		// was deselected.
		shuttleSector = applyVehicle(building, shuttleSector, 2, 3, 0b111);
		requireLayout(building, shuttleSector, 0, 2, 3, 0b111, stops, expected,
			{ 0, 1, 2, 4, 5, 6, 20, 21, 22, 24, 25, 26 },
			"Shuttle with cell 0 re-selected");
		require(findShuttleDoor(building, shuttleSector, 0, 0, 0)->getOpenStyle()
			== Style::OpenUp,
			"A discarded style came back when its carriage cell was re-selected");
		for (auto const& slot : std::vector<Slot>{
			{ 0, 0, 0, Style::OpenRight }, { 0, 1, 0, Style::OpenApart },
			{ 1, 0, 0, Style::OpenLeft }, { 1, 1, 0, Style::OpenRight } })
		{
			require(building.setShuttleDoorOpenStyle(shuttleSector, slot.stop, slot.car, 0,
				slot.style, &diagnostic),
				("Restyling a re-selected Door was refused: " + diagnostic).c_str());
			expected[std::array<uint32_t, 3>{ slot.stop, slot.car, 0 }] = slot.style;
		}
		requireLayout(building, shuttleSector, 0, 2, 3, 0b111, stops, expected,
			{ 0, 1, 2, 4, 5, 6, 20, 21, 22, 24, 25, 26 },
			"Shuttle with every cell of width 3 styled");

		// Widening the carriages moves every style to the physical Door its own
		// identity now addresses.  The style authored for stop 0, carriage 1,
		// cell 2 moves onto the cell 7 landing, which does not exist, so it is
		// dropped there rather than leaking onto cell 6 or cell 8; the new cell 3
		// generates OpenUp.
		expected.erase(std::array<uint32_t, 3>{ 0u, 1u, 2u });
		shuttleSector = applyVehicle(building, shuttleSector, 2, 4, 0b1111);
		requireLayout(building, shuttleSector, 0, 2, 4, 0b1111, stops, expected,
			{ 0, 1, 2, 3, 5, 6, 8, 20, 21, 22, 23, 25, 26, 27, 28 },
			"Shuttle on wider carriages");

		// The reconciled result round-trips through save/load.
		{
			auto const loaded = loadYaml(snapshotYaml(building));
			requireLayout(*loaded, shuttleSector, 0, 2, 4, 0b1111, stops, expected,
				{ 0, 1, 2, 3, 5, 6, 8, 20, 21, 22, 23, 25, 26, 27, 28 },
				"loaded Shuttle after the vehicle changes");
		}
	}

	// Ticket #103: the public vehicle-edit API authors a complete vehicle, so a
	// zero carriage count, carriage width, or door mask must be rejected rather
	// than consumed as the internal keep-current sentinel.
	void shuttleVehicleEditsRejectZeroValuedFields()
	{
		core::Building building("Shuttle vehicle edit validation", 32, 2);
		building.addCorridor(0, 0, 0, 31, 1);
		core::Building::CreateShuttleOptions options{ 1, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101;
		auto const created = building.addShuttle(1, 0, 0, 27, options);
		building.finishBuild();
		building.pauseSimulation();
		auto const index = created.shuttle.sector->getIndex();

		auto const rejected = [&](uint32_t numCars, uint32_t carWidth, uint32_t doorMask,
			std::string const& needle, std::string const& field)
		{
			auto const plan = building.planEditShuttleVehicle(index, numCars, carWidth, doorMask);
			require(!plan.valid,
				("A zero-valued " + field + " was accepted as a Shuttle vehicle edit").c_str());
			require(plan.diagnostic.find(needle) != std::string::npos,
				("The rejected " + field + " carried no useful diagnostic: "
					+ plan.diagnostic).c_str());
		};
		rejected(0, 3, 0b101, "at least one carriage", "carriage count");
		rejected(1, 0, 0b101, "between 3 and 5", "carriage width");
		rejected(1, 3, 0, "at least one cell", "door mask");

		// The pre-existing range diagnostics stay enforced through the public API.
		auto const tooNarrow = building.planEditShuttleVehicle(index, 1, 2, 0b11);
		require(!tooNarrow.valid
			&& tooNarrow.diagnostic.find("between 3 and 5") != std::string::npos,
			("An out-of-range carriage width was accepted: "
				+ tooNarrow.diagnostic).c_str());
		auto const maskBeyondWidth = building.planEditShuttleVehicle(index, 1, 3, 0b1000);
		require(!maskBeyondWidth.valid
			&& maskBeyondWidth.diagnostic.find("within the carriage width") != std::string::npos,
			("A door mask reaching past the carriage width was accepted: "
				+ maskBeyondWidth.diagnostic).c_str());

		// The sentinel stays private to the track-resize path, which keeps the
		// current vehicle layout.
		auto const trackOnly = building.planResizeShuttle(index, 0, 0, 27);
		require(trackOnly.valid && trackOnly.numCars == 1 && trackOnly.carWidth == 3
			&& trackOnly.doorMask == 0b101,
			("A track-only Shuttle edit did not preserve the authored vehicle layout: "
				+ trackOnly.diagnostic).c_str());

		// A valid vehicle edit still plans and applies end to end.
		auto const valid = building.planEditShuttleVehicle(index, 2, 4, 0b1001);
		require(valid.valid, ("A valid Shuttle vehicle edit was refused: " + valid.diagnostic).c_str());
		require(valid.numCars == 2 && valid.carWidth == 4 && valid.doorMask == 0b1001,
			"The valid vehicle plan did not carry the requested layout");
		auto const edited = building.applyShuttleEdit(valid);
		auto const transit = std::dynamic_pointer_cast<const core::ShuttleTransit>(
			building.getSector(edited));
		require(transit != nullptr, "The edited Shuttle is no longer a Shuttle Transit");
		core::Building::CreateShuttleOptions applied{};
		require(building.getShuttleOptions(transit->getShuttle().get(), applied)
			&& applied.numCars == 2 && applied.carWidth == 4 && applied.doorMask == 0b1001,
			"The applied Shuttle vehicle does not match the plan");
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
	char const* roomNames[] = { "Corridor", "Basement", "Deep Cellar", "Catacomb", "Oubliette" };
	// One name per Layer the loop can build. Deriving the bound from the array
	// keeps the scan from reading past the end of roomNames, which clang's
	// Release codegen surfaced as a crash where GCC's layout happened to survive.
	uint32_t const maxLayerCount = static_cast<uint32_t>(sizeof(roomNames) / sizeof(roomNames[0]));

	for (uint32_t layerCount = 2; layerCount <= maxLayerCount; ++layerCount)
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
#if !defined(_WIN32)
	saveThroughSymlinkUpdatesItsTarget();
	savePreservesExistingFilePermissions();
#endif
	failedSavePreservesUnsavedChangesState();
	malformedValuesAndInvalidUsageThrowUsefulErrors();
	buildingRoundTripsAuthoredStateAndAgents();
	agentRestoreRejectsMalformedPositions();
	agentRestoreRejectsBackgroundAndUnreachableDestination();
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
	doorOpeningStyleIsAuthoredPersistedAndLegacyDefaulted();
	doorDeckSpanPersistsAndLegacyRecordsStayOneDeckTall();
	doorOpenLeftPersistsThroughEveryEditorPath();
	doorOpenRightPersistsThroughEveryEditorPath();
	doorOpenApartPersistsThroughEveryEditorPath();
	liftDoorsDefaultToOpenApartWhileOtherDoorsKeepOpenUp();
	doorStyleMapsAdvanceTheSchemaVersionAndLegacySixStillLoads();
	liftStopDoorStyleOverridesArePerStopAndPersist();
	liftCreationStopDoorStylesAreAuthoredAndPersist();
	liftShortStopDoorStyleVectorEditPreservesEarlierOverrides();
	liftDoorStylesFollowStopsWhenTheLiftMovesOrResizes();
	liftDoorStylesReconcileWhenStopsChange();
	shuttleDoorStyleOverridesAreIndividualAndPersist();
	shuttleDoorStylesSurviveShuttleMovement();
	shuttleDoorStylesReconcileWhenStopsChange();
	shuttleDoorStylesReconcileWhenCarriageAndDoorLayoutChanges();
	shuttleVehicleEditsRejectZeroValuedFields();
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
