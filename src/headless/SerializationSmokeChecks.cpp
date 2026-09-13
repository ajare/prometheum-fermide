#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

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
	serializableTracksModificationState();
}
