#include "core/Building.h"
#include "core/SerializationException.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace core
{
	bool Building::childrenModified() const
	{
		return std::any_of(mAgents.entries().begin(), mAgents.entries().end(),
			[](auto const& entry) { return entry.second->isModified(); });
	}

	void Building::recordConstruction(ConstructionRecord record)
	{
		if (!mDeserializingConstruction)
		{
			mConstructionRecords.push_back(std::move(record));
		}
	}

	void Building::serializeImpl(Serializer& serializer, SerializationWorkData& workData) const
	{
		serializer.beginMap("building");
		serializer.writeUint32("version", 1);
		serializer.writeString("name", mName);
		serializer.writeUint32("cellsWide", mCellsWide);
		serializer.writeUint32("decksHigh", mDecksHigh);

		serializer.beginArray("construction");
		for (auto const& record : mConstructionRecords)
		{
			serializer.beginMap("");
			serializer.writeUint32("kind", static_cast<uint32_t>(record.type));
			serializer.writeString("name", record.name);
			serializer.writeUint32("a", record.a);
			serializer.writeUint32("b", record.b);
			serializer.writeUint32("c", record.c);
			serializer.writeUint32("d", record.d);
			serializer.writeUint32("e", record.e);
			serializer.writeUint32("f", record.f);
			serializer.writeUint32("g", record.g);
			serializer.writeInt32("i", record.i);
			serializer.writeInt32("j", record.j);
			serializer.writeFloat("x", record.x);
			serializer.writeFloat("y", record.y);
			serializer.writeBool("p", record.p);
			serializer.writeBool("q", record.q);
			serializer.beginArray("values", false);
			for (auto value : record.values)
			{
				serializer.writeUint32("", value);
			}
			serializer.endArray();
			serializer.endMap();
		}
		serializer.endArray();

		serializer.beginArray("agents");
		for (auto const& [id, agent] : mAgents.entries())
		{
			auto const* sector = agent->getSector();
			if (!sector)
			{
				throw SerializationException("Cannot serialize a Building-owned Agent without a Sector");
			}
			serializer.beginMap("");
			serializer.writeUint64("id", id.value);
			agent->serialize(serializer, workData);
			serializer.writeUint32("sector", sector->getIndex());
			serializer.writeFloat("localX", agent->getLocalPosition().x);
			serializer.writeFloat("localY", agent->getLocalPosition().y);
			serializer.endMap();
		}
		serializer.endArray();
		serializer.endMap();
	}

	bool Building::deserializeImpl(Serializer& serializer, SerializationWorkData& workData)
	{
		serializer.beginMap("building");
		auto const version = serializer.readUint32("version");
		if (version != 1)
		{
			throw SerializationException("Unsupported Building serialization version");
		}
		auto name = serializer.readString("name");
		auto const cellsWide = serializer.readUint32("cellsWide");
		auto const decksHigh = serializer.readUint32("decksHigh");
		if (cellsWide == 0 || decksHigh == 0)
		{
			throw SerializationException("Building dimensions must be positive");
		}

		std::vector<ConstructionRecord> records;
		serializer.beginArray("construction");
		while (serializer.nextArrayItem())
		{
			serializer.beginMap("");
			ConstructionRecord record;
			auto const kind = serializer.readUint32("kind");
			if (kind > static_cast<uint32_t>(ConstructionType::RemoveMarker))
			{
				throw SerializationException("Unknown Building construction record kind");
			}
			record.type = static_cast<ConstructionType>(kind);
			record.name = serializer.readString("name");
			record.a = serializer.readUint32("a");
			record.b = serializer.readUint32("b");
			record.c = serializer.readUint32("c");
			record.d = serializer.readUint32("d");
			record.e = serializer.readUint32("e");
			record.f = serializer.readUint32("f");
			record.g = serializer.readUint32("g");
			record.i = serializer.readInt32("i");
			record.j = serializer.readInt32("j");
			record.x = serializer.readFloat("x");
			record.y = serializer.readFloat("y");
			record.p = serializer.readBool("p");
			record.q = serializer.readBool("q");
			serializer.beginArray("values", false);
			while (serializer.nextArrayItem())
			{
				record.values.push_back(serializer.readUint32());
			}
			serializer.endArray();
			serializer.endMap();
			records.push_back(std::move(record));
		}
		serializer.endArray();

		resetForDeserialization(std::move(name), cellsWide, decksHigh);
		mDeserializingConstruction = true;
		try
		{
			for (auto const& record : records)
			{
				applyConstructionRecord(record);
			}
			finishBuild();
		}
		catch (...)
		{
			mDeserializingConstruction = false;
			throw;
		}
		mDeserializingConstruction = false;
		mConstructionRecords = std::move(records);

		serializer.beginArray("agents");
		while (serializer.nextArrayItem())
		{
			serializer.beginMap("");
			auto const id = AgentId{ serializer.readUint64("id") };
			if (!id)
			{
				throw SerializationException("Serialized Agent ID cannot be zero");
			}
			auto agent = std::make_unique<Agent>("");
			agent->deserialize(serializer, workData);
			auto const sectorIndex = serializer.readUint32("sector");
			auto const localX = serializer.readFloat("localX");
			auto const localY = serializer.readFloat("localY");
			serializer.endMap();

			if (mAgents.find(id))
			{
				throw SerializationException("Serialized Agent IDs must be unique");
			}
			auto sector = _getSector(sectorIndex);
			auto* rawAgent = agent.get();
			rawAgent->attachToBuilding(this);
			rawAgent->mPosition = SectorPosition(sector.get(), localX, localY);
			sector->mAgents.insert(rawAgent);
			mAgents.restore(id, std::move(agent));
			mAgentIds.emplace(rawAgent, id);
		}
		serializer.endArray();
		serializer.endMap();
		return true;
	}

	void Building::resetForDeserialization(std::string name, uint32_t cellsWide, uint32_t decksHigh)
	{
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (auto* sector = const_cast<Sector*>(agent->getSector()))
			{
				sector->mAgents.erase(agent.get());
			}
		}
		mAgents = {};
		mAgentIds.clear();
		mInteractionPoints = {};
		mInteractionRequests = {};
		mDeviceOperations = {};
		mTraversalResources = {};
		mTraversalRequests = {};
		mTraversalPermits = {};
		mSectors.clear();
		mConstructionRecords.clear();

		mName = std::move(name);
		mCellsWide = cellsWide;
		mDecksHigh = decksHigh;
		for (uint32_t layer = 0; layer < CORE_NUM_LAYERS; ++layer)
		{
			mLayers[layer] = std::make_shared<Layer>(this, cellsWide, decksHigh, layer);
		}
		mGraph = std::make_shared<Graph>(this);
		mSimulationTick = 0;
		mNextEventSequence = 1;
		mNextQueueTicketValue = 1;
		mNextDoorOpenLeaseValue = 1;
		mAccumulatedTime = 0.0;
		mCurrentPhase = SimulationPhase::None;
		mEvents.clear();
		mTraversalWaitingPolicy = {};
		mBuildFinished = false;
		mSimulationPaused = false;
		mTopologyDirty = true;
		mTopologyValid = false;
		mTopologyGeneration = 0;
		mTopologyDiagnostic.clear();
		mPausedPathIntents.clear();
		mBuildLog.clear();
	}

	void Building::applyConstructionRecord(ConstructionRecord const& record)
	{
		switch (record.type)
		{
		case ConstructionType::Corridor:
			addCorridor(record.a, record.b, record.c, record.d);
			break;
		case ConstructionType::Room:
			addRoom(record.name, record.a, record.b, record.c, record.d, record.e, record.x);
			break;
		case ConstructionType::Ladder:
			addLadder(record.a, record.b,
				{ record.c, record.p, record.q, record.x, record.d });
			break;
		case ConstructionType::Staircase:
			addStaircase(record.a, record.b,
				{ record.c, record.i, record.d, record.e });
			break;
		case ConstructionType::Lift:
			addLift(record.a, record.b,
				{ record.c, record.values, record.d, record.x, record.y });
			break;
		case ConstructionType::Shuttle:
			addShuttle(record.a, record.b, record.c,
				{ record.d, record.e, record.values, record.f, record.g, record.x, record.y });
			break;
		case ConstructionType::Door:
			addSectorDoor(record.a, record.b,
				{ record.c, { record.p, record.q }, static_cast<DoorActivationMode>(record.i), record.x, record.d });
			break;
		case ConstructionType::Window:
			addSectorWindow(record.a, record.b, record.c, record.d, record.e,
				{ record.p, static_cast<Window::State>(record.i), static_cast<Window::Style>(record.j) });
			break;
		case ConstructionType::BulkheadDoor:
			addSectorBulkheadDoor(record.a, record.b, record.c, record.i,
				{ { record.p, record.q }, static_cast<DoorActivationMode>(record.j), record.x, record.d });
			break;
		case ConstructionType::LightSwitch:
			addSectorLightSwitch(record.a, record.b);
			break;
		case ConstructionType::ForceBridge:
			addSectorForceBridge(record.a, record.b, record.c,
				{ record.d, record.i, record.p, record.q, record.e });
			break;
		case ConstructionType::SectorLadder:
			addSectorLadder(record.a, record.b, record.c,
				{ record.d, record.p, record.q, record.x, record.e });
			break;
		case ConstructionType::PlatformLift:
			addSectorPlatformLift(record.a, record.b, record.c,
				{ record.d, record.values, record.e, record.x, record.y });
			break;
		case ConstructionType::Walkway:
			addSectorWalkway(record.a, record.b, record.c);
			break;
		case ConstructionType::Marker:
			addSectorMarker(record.a, record.b, record.x);
			break;
		case ConstructionType::RemoveWall:
			removeLocationWall(record.a, record.b, record.i);
			break;
		case ConstructionType::RemoveMarker:
			if (!removeSectorMarker(record.a, record.b))
				throw SerializationException("Could not replay Marker deletion");
			break;
		}
	}
}
