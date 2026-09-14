#include "core/Building.h"
#include "core/SerializationException.h"
#include "core/Exceptions.h"
#include "core/Transit.h"
#include "core/LiftTransit.h"
#include "core/Location.h"
#include "core/DoorSectorObject.h"
#include "core/MarkerSectorObject.h"
#include "core/WindowSectorObject.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <stdexcept>
#include <utility>

namespace core
{
	using namespace std;

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

	string Building::constructionTypeName(ConstructionType type)
	{
		switch (type)
		{
		case ConstructionType::Corridor: return "corridor";
		case ConstructionType::Room: return "room";
		case ConstructionType::Ladder: return "ladder";
		case ConstructionType::Staircase: return "staircase";
		case ConstructionType::Lift: return "lift";
		case ConstructionType::Shuttle: return "shuttle";
		case ConstructionType::Door: return "door";
		case ConstructionType::Window: return "window";
		case ConstructionType::BulkheadDoor: return "bulkheadDoor";
		case ConstructionType::LightSwitch: return "lightSwitch";
		case ConstructionType::ForceBridge: return "forceBridge";
		case ConstructionType::SectorLadder: return "sectorLadder";
		case ConstructionType::PlatformLift: return "platformLift";
		case ConstructionType::Walkway: return "walkway";
		case ConstructionType::Marker: return "marker";
		case ConstructionType::RemoveWall: return "removeWall";
		case ConstructionType::RemoveMarker: return "removeMarker";
		case ConstructionType::ObjectTombstone: return "objectTombstone";
		}
		throw SerializationException("Unknown Building construction record type");
	}

	Building::ConstructionType Building::constructionTypeFromName(string const& name)
	{
		for (uint32_t value = 0; value <= static_cast<uint32_t>(ConstructionType::ObjectTombstone); ++value)
		{
			auto const type = static_cast<ConstructionType>(value);
			if (constructionTypeName(type) == name) return type;
		}
		throw SerializationException(format("Unknown Building construction record type: {}", name));
	}

	void Building::serializeConstructionRecord(Serializer& serializer, ConstructionRecord const& record) const
	{
		auto writeStops = [&]
		{
			serializer.beginArray("stopOffsets", false);
			for (auto value : record.values) serializer.writeUint32("", value);
			serializer.endArray();
		};
		auto layerName = [](uint32_t layer)
		{
			if (layer == CORE_LAYER_FORE) return "fore";
			if (layer == CORE_LAYER_BACK) return "back";
			throw SerializationException("Cannot serialize an unknown Building layer");
		};
		auto sideName = [](int side)
		{
			if (side == CORE_SIDE_LEFT) return "left";
			if (side == CORE_SIDE_RIGHT) return "right";
			throw SerializationException("Cannot serialize an unknown side");
		};
		auto activationName = [](int32_t mode)
		{
			switch (static_cast<DoorActivationMode>(mode))
			{
			case DoorActivationMode::Automatic: return "automatic";
			case DoorActivationMode::Manual: return "manual";
			case DoorActivationMode::RemoteControlled: return "remoteControlled";
			case DoorActivationMode::Unavailable: return "unavailable";
			}
			throw SerializationException("Cannot serialize an unknown Door activation mode");
		};

		serializer.writeString("type", constructionTypeName(record.type));
		switch (record.type)
		{
		case ConstructionType::Corridor:
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("cellsWide", record.c); serializer.writeUint32("decksHigh", record.d); break;
		case ConstructionType::Room:
			serializer.writeString("name", record.name); serializer.writeString("layer", layerName(record.a));
			serializer.writeUint32("y", record.b); serializer.writeUint32("x", record.c);
			serializer.writeUint32("cellsWide", record.d); serializer.writeUint32("decksHigh", record.e);
			serializer.writeFloat("topDeckHeight", record.x); break;
		case ConstructionType::Ladder:
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("decksHigh", record.c); serializer.writeBool("extensible", record.p);
			serializer.writeBool("startExtended", record.q); serializer.writeFloat("agentSpacing", record.x);
			serializer.writeUint32("directionalBatchLimit", record.d); break;
		case ConstructionType::Staircase:
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("decksHigh", record.c); serializer.writeString("mountSide", sideName(record.i));
			serializer.writeUint32("directionalCapacity", record.d);
			serializer.writeUint32("directionalBatchLimit", record.e); break;
		case ConstructionType::Lift:
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("cellsWide", record.c); serializer.writeUint32("decksHigh", record.e);
			writeStops(); serializer.writeUint32("capacity", record.d);
			serializer.writeFloat("minimumDwellSeconds", record.x);
			serializer.writeFloat("maximumBoardingSeconds", record.y);
			serializer.writeUint32("initialStop", record.g); break;
		case ConstructionType::Shuttle:
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("cellsWide", record.c); serializer.writeUint32("numCars", record.d);
			serializer.writeUint32("carWidth", record.e); writeStops();
			serializer.writeUint32("initialStop", record.f); serializer.writeUint32("capacityPerCarriage", record.g);
			serializer.writeFloat("minimumDwellSeconds", record.x);
			serializer.writeFloat("maximumBoardingSeconds", record.y);
			serializer.writeBool("allowPartialLandings", record.p); break;
		case ConstructionType::Door:
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("width", record.c); serializer.writeBool("foreControl", record.p);
			serializer.writeBool("backControl", record.q); serializer.writeString("activationMode", activationName(record.i));
			serializer.writeFloat("holdOpenSeconds", record.x); serializer.writeUint32("crossingLanes", record.d); break;
		case ConstructionType::Window:
		{
			static char const* states[] = { "open", "opening", "closed", "closing", "broken", "frosted", "frosting", "unfrosting", "tinted", "tinting", "untinting" };
			static char const* styles[] = { "clear", "tinted", "frosted" };
			if (record.i < 0 || record.i >= static_cast<int32_t>(size(states)) || record.j < 0 || record.j >= static_cast<int32_t>(size(styles)))
				throw SerializationException("Cannot serialize an unknown Window state or style");
			serializer.writeString("layer", layerName(record.a)); serializer.writeUint32("y", record.b);
			serializer.writeUint32("x", record.c); serializer.writeUint32("cellsWide", record.d);
			serializer.writeUint32("decksHigh", record.e); serializer.writeBool("traversable", record.p);
			serializer.writeString("initialState", states[record.i]); serializer.writeString("style", styles[record.j]); break;
		}
		case ConstructionType::BulkheadDoor:
			serializer.writeString("layer", layerName(record.a)); serializer.writeUint32("y", record.b);
			serializer.writeUint32("x", record.c); serializer.writeString("side", sideName(record.i));
			serializer.writeBool("foreControl", record.p); serializer.writeBool("backControl", record.q);
			serializer.writeString("activationMode", activationName(record.j));
			serializer.writeFloat("holdOpenSeconds", record.x); serializer.writeUint32("crossingLanes", record.d); break;
		case ConstructionType::LightSwitch:
			serializer.writeUint32("sectorIndex", record.a); serializer.writeUint32("xOffset", record.b); break;
		case ConstructionType::ForceBridge:
			serializer.writeUint32("sectorIndex", record.a); serializer.writeUint32("deckIndex", record.b);
			serializer.writeUint32("xOffset", record.c); serializer.writeUint32("width", record.d);
			serializer.writeString("fromSide", sideName(record.i)); serializer.writeBool("extensible", record.p);
			serializer.writeBool("startExtended", record.q); serializer.writeUint32("controlCount", record.e); break;
		case ConstructionType::SectorLadder:
			serializer.writeUint32("sectorIndex", record.a); serializer.writeUint32("deckIndex", record.b);
			serializer.writeUint32("xOffset", record.c); serializer.writeUint32("decksHigh", record.d);
			serializer.writeBool("extensible", record.p); serializer.writeBool("startExtended", record.q);
			serializer.writeFloat("agentSpacing", record.x); serializer.writeUint32("directionalBatchLimit", record.e); break;
		case ConstructionType::PlatformLift:
			serializer.writeUint32("sectorIndex", record.a); serializer.writeUint32("deckIndex", record.b);
			serializer.writeUint32("xOffset", record.c); serializer.writeUint32("cellsWide", record.d);
			writeStops(); serializer.writeUint32("capacity", record.e);
			serializer.writeFloat("minimumDwellSeconds", record.x);
			serializer.writeFloat("maximumBoardingSeconds", record.y); break;
		case ConstructionType::Walkway:
			serializer.writeUint32("sectorIndex", record.a); serializer.writeUint32("deckIndex", record.b);
			serializer.writeUint32("xOffset", record.c); break;
		case ConstructionType::Marker:
			serializer.writeUint32("sectorIndex", record.a); serializer.writeUint32("deckIndex", record.b);
			serializer.writeFloat("xOffset", record.x); break;
		case ConstructionType::RemoveWall:
			serializer.writeUint32("sectorIndex", record.a); serializer.writeUint32("deckIndex", record.b);
			serializer.writeString("side", sideName(record.i)); break;
		case ConstructionType::RemoveMarker:
			serializer.writeUint32("sectorIndex", record.a); serializer.writeUint32("objectIndex", record.b); break;
		case ConstructionType::ObjectTombstone:
			serializer.writeUint32("sectorIndex", record.a); break;
		}
	}

	void Building::serializeImpl(Serializer& serializer, SerializationWorkData& workData) const
	{
		serializer.beginMap("building");
		serializer.writeUint32("version", 2);
		serializer.writeString("name", mName);
		serializer.writeUint32("cellsWide", mCellsWide);
		serializer.writeUint32("decksHigh", mDecksHigh);

		serializer.beginArray("construction");
		for (auto const& record : mConstructionRecords)
		{
			serializer.beginMap("");
			serializeConstructionRecord(serializer, record);
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

	Building::ConstructionRecord Building::deserializeConstructionRecord(
		Serializer& serializer, uint32_t version) const
	{
		ConstructionRecord record;
		if (version == 1)
		{
			auto const kind = serializer.readUint32("kind");
			if (kind > static_cast<uint32_t>(ConstructionType::ObjectTombstone))
				throw SerializationException("Unknown Building construction record kind");
			record.type = static_cast<ConstructionType>(kind);
			record.name = serializer.readString("name");
			record.a = serializer.readUint32("a"); record.b = serializer.readUint32("b");
			record.c = serializer.readUint32("c"); record.d = serializer.readUint32("d");
			record.e = serializer.readUint32("e"); record.f = serializer.readUint32("f");
			record.g = serializer.readUint32("g"); record.i = serializer.readInt32("i");
			record.j = serializer.readInt32("j"); record.x = serializer.readFloat("x");
			record.y = serializer.readFloat("y"); record.p = serializer.readBool("p");
			record.q = serializer.readBool("q");
			serializer.beginArray("values", false);
			while (serializer.nextArrayItem()) record.values.push_back(serializer.readUint32());
			serializer.endArray();
			return record;
		}

		auto readLayer = [&](char const* field)
		{
			auto const value = serializer.readString(field);
			if (value == "fore") return static_cast<uint32_t>(CORE_LAYER_FORE);
			if (value == "back") return static_cast<uint32_t>(CORE_LAYER_BACK);
			throw SerializationException(format("Unknown layer: {}", value));
		};
		auto readSide = [&](char const* field)
		{
			auto const value = serializer.readString(field);
			if (value == "left") return CORE_SIDE_LEFT;
			if (value == "right") return CORE_SIDE_RIGHT;
			throw SerializationException(format("Unknown side: {}", value));
		};
		auto readActivation = [&](char const* field)
		{
			auto const value = serializer.readString(field);
			if (value == "automatic") return static_cast<int32_t>(DoorActivationMode::Automatic);
			if (value == "manual") return static_cast<int32_t>(DoorActivationMode::Manual);
			if (value == "remoteControlled") return static_cast<int32_t>(DoorActivationMode::RemoteControlled);
			if (value == "unavailable") return static_cast<int32_t>(DoorActivationMode::Unavailable);
			throw SerializationException(format("Unknown Door activation mode: {}", value));
		};
		auto readStops = [&]
		{
			serializer.beginArray("stopOffsets", false);
			while (serializer.nextArrayItem()) record.values.push_back(serializer.readUint32());
			serializer.endArray();
		};

		record.type = constructionTypeFromName(serializer.readString("type"));
		switch (record.type)
		{
		case ConstructionType::Corridor:
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("cellsWide"); record.d = serializer.readUint32("decksHigh"); break;
		case ConstructionType::Room:
			record.name = serializer.readString("name"); record.a = readLayer("layer");
			record.b = serializer.readUint32("y"); record.c = serializer.readUint32("x");
			record.d = serializer.readUint32("cellsWide"); record.e = serializer.readUint32("decksHigh");
			record.x = serializer.readFloat("topDeckHeight"); break;
		case ConstructionType::Ladder:
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("decksHigh"); record.p = serializer.readBool("extensible");
			record.q = serializer.readBool("startExtended"); record.x = serializer.readFloat("agentSpacing");
			record.d = serializer.readUint32("directionalBatchLimit"); break;
		case ConstructionType::Staircase:
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("decksHigh"); record.i = readSide("mountSide");
			record.d = serializer.readUint32("directionalCapacity");
			record.e = serializer.readUint32("directionalBatchLimit"); break;
		case ConstructionType::Lift:
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("cellsWide"); record.e = serializer.readUint32("decksHigh");
			readStops(); record.d = serializer.readUint32("capacity");
			record.x = serializer.readFloat("minimumDwellSeconds");
			record.y = serializer.readFloat("maximumBoardingSeconds");
			record.g = serializer.readUint32("initialStop"); break;
		case ConstructionType::Shuttle:
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("cellsWide"); record.d = serializer.readUint32("numCars");
			record.e = serializer.readUint32("carWidth"); readStops();
			record.f = serializer.readUint32("initialStop"); record.g = serializer.readUint32("capacityPerCarriage");
			record.x = serializer.readFloat("minimumDwellSeconds");
			record.y = serializer.readFloat("maximumBoardingSeconds");
			record.p = serializer.readBool("allowPartialLandings"); break;
		case ConstructionType::Door:
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("width"); record.p = serializer.readBool("foreControl");
			record.q = serializer.readBool("backControl"); record.i = readActivation("activationMode");
			record.x = serializer.readFloat("holdOpenSeconds"); record.d = serializer.readUint32("crossingLanes"); break;
		case ConstructionType::Window:
		{
			static char const* states[] = { "open", "opening", "closed", "closing", "broken", "frosted", "frosting", "unfrosting", "tinted", "tinting", "untinting" };
			static char const* styles[] = { "clear", "tinted", "frosted" };
			record.a = readLayer("layer"); record.b = serializer.readUint32("y");
			record.c = serializer.readUint32("x"); record.d = serializer.readUint32("cellsWide");
			record.e = serializer.readUint32("decksHigh"); record.p = serializer.readBool("traversable");
			auto const state = serializer.readString("initialState"); auto const style = serializer.readString("style");
			auto stateIt = find(begin(states), end(states), state); auto styleIt = find(begin(styles), end(styles), style);
			if (stateIt == end(states) || styleIt == end(styles)) throw SerializationException("Unknown Window state or style");
			record.i = static_cast<int32_t>(distance(begin(states), stateIt));
			record.j = static_cast<int32_t>(distance(begin(styles), styleIt)); break;
		}
		case ConstructionType::BulkheadDoor:
			record.a = readLayer("layer"); record.b = serializer.readUint32("y");
			record.c = serializer.readUint32("x"); record.i = readSide("side");
			record.p = serializer.readBool("foreControl"); record.q = serializer.readBool("backControl");
			record.j = readActivation("activationMode"); record.x = serializer.readFloat("holdOpenSeconds");
			record.d = serializer.readUint32("crossingLanes"); break;
		case ConstructionType::LightSwitch:
			record.a = serializer.readUint32("sectorIndex"); record.b = serializer.readUint32("xOffset"); break;
		case ConstructionType::ForceBridge:
			record.a = serializer.readUint32("sectorIndex"); record.b = serializer.readUint32("deckIndex");
			record.c = serializer.readUint32("xOffset"); record.d = serializer.readUint32("width");
			record.i = readSide("fromSide"); record.p = serializer.readBool("extensible");
			record.q = serializer.readBool("startExtended"); record.e = serializer.readUint32("controlCount"); break;
		case ConstructionType::SectorLadder:
			record.a = serializer.readUint32("sectorIndex"); record.b = serializer.readUint32("deckIndex");
			record.c = serializer.readUint32("xOffset"); record.d = serializer.readUint32("decksHigh");
			record.p = serializer.readBool("extensible"); record.q = serializer.readBool("startExtended");
			record.x = serializer.readFloat("agentSpacing"); record.e = serializer.readUint32("directionalBatchLimit"); break;
		case ConstructionType::PlatformLift:
			record.a = serializer.readUint32("sectorIndex"); record.b = serializer.readUint32("deckIndex");
			record.c = serializer.readUint32("xOffset"); record.d = serializer.readUint32("cellsWide");
			readStops(); record.e = serializer.readUint32("capacity");
			record.x = serializer.readFloat("minimumDwellSeconds");
			record.y = serializer.readFloat("maximumBoardingSeconds"); break;
		case ConstructionType::Walkway:
			record.a = serializer.readUint32("sectorIndex"); record.b = serializer.readUint32("deckIndex");
			record.c = serializer.readUint32("xOffset"); break;
		case ConstructionType::Marker:
			record.a = serializer.readUint32("sectorIndex"); record.b = serializer.readUint32("deckIndex");
			record.x = serializer.readFloat("xOffset"); break;
		case ConstructionType::RemoveWall:
			record.a = serializer.readUint32("sectorIndex"); record.b = serializer.readUint32("deckIndex");
			record.i = readSide("side"); break;
		case ConstructionType::RemoveMarker:
			record.a = serializer.readUint32("sectorIndex"); record.b = serializer.readUint32("objectIndex"); break;
		case ConstructionType::ObjectTombstone:
			record.a = serializer.readUint32("sectorIndex"); break;
		}
		return record;
	}

	bool Building::deserializeImpl(Serializer& serializer, SerializationWorkData& workData)
	{
		serializer.beginMap("building");
		auto const version = serializer.readUint32("version");
		if (version != 1 && version != 2)
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
			auto record = deserializeConstructionRecord(serializer, version);
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
		mPhysicalControlPlacements.clear();

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
				{ record.c, record.values, record.d, record.x, record.y, record.g, record.e });
			break;
		case ConstructionType::Shuttle:
			addShuttle(record.a, record.b, record.c,
				{ record.d, record.e, record.values, record.f, record.g, record.x, record.y, record.p });
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
		case ConstructionType::ObjectTombstone:
			_getSector(record.a)->addSectorObject(nullptr);
			break;
		}
	}

	vector<Building::ConstructionRecord> Building::canonicalConstructionRecords(
		vector<ConstructionRecord> records) const
	{
		auto createsSector = [](ConstructionType type)
		{
			return type == ConstructionType::Corridor || type == ConstructionType::Room
				|| type == ConstructionType::Ladder || type == ConstructionType::Staircase
				|| type == ConstructionType::Lift || type == ConstructionType::Shuttle;
		};
		auto isLocation = [](ConstructionType type)
		{
			return type == ConstructionType::Corridor || type == ConstructionType::Room;
		};
		auto referencesSector = [](ConstructionType type)
		{
			return type == ConstructionType::LightSwitch || type == ConstructionType::ForceBridge
				|| type == ConstructionType::SectorLadder || type == ConstructionType::PlatformLift
				|| type == ConstructionType::Walkway || type == ConstructionType::Marker
				|| type == ConstructionType::RemoveWall || type == ConstructionType::RemoveMarker
				|| type == ConstructionType::ObjectTombstone;
		};
		struct Item { ConstructionRecord record; uint32_t oldSector{ ~0u }; };
		vector<Item> locations, transits, other;
		uint32_t oldSector = 0;
		for (auto& record : records)
		{
			bool const producer = createsSector(record.type);
			Item item{ std::move(record), producer ? oldSector++ : ~0u };
			if (isLocation(item.record.type)) locations.push_back(std::move(item));
			else if (createsSector(item.record.type)) transits.push_back(std::move(item));
			else other.push_back(std::move(item));
		}
		vector<Item> ordered;
		ordered.reserve(records.size());
		for (auto& item : locations) ordered.push_back(std::move(item));
		for (auto& item : transits) ordered.push_back(std::move(item));
		for (auto& item : other) ordered.push_back(std::move(item));
		vector<uint32_t> sectorMap(oldSector, ~0u);
		uint32_t nextSector = 0;
		for (auto const& item : ordered)
			if (item.oldSector != ~0u) sectorMap[item.oldSector] = nextSector++;
		vector<ConstructionRecord> result;
		result.reserve(ordered.size());
		for (auto& item : ordered)
		{
			if (referencesSector(item.record.type) && item.record.a < sectorMap.size())
				item.record.a = sectorMap[item.record.a];
			result.push_back(std::move(item.record));
		}
		return result;
	}

	bool Building::prepareLiftEdit(LiftEditPlan const& plan,
		vector<ConstructionRecord>& records, string& diagnostic) const
	{
		records = mConstructionRecords;
		uint32_t producerIndex = 0;
		auto found = records.end();
		for (auto it = records.begin(); it != records.end(); ++it)
		{
			bool producer = it->type == ConstructionType::Corridor || it->type == ConstructionType::Room
				|| it->type == ConstructionType::Ladder || it->type == ConstructionType::Staircase
				|| it->type == ConstructionType::Lift || it->type == ConstructionType::Shuttle;
			if (!producer) continue;
			if (producerIndex++ == plan.sectorIndex) { found = it; break; }
		}
		if (found == records.end() || found->type != ConstructionType::Lift)
		{
			diagnostic = "The selected Lift no longer has an authored definition";
			return false;
		}
		if (plan.remove) records.erase(found);
		else
		{
			found->a = plan.y; found->b = plan.x; found->c = plan.cellsWide;
			found->e = plan.decksHigh; found->values = plan.stopOffsets;
			auto oldPosition = 0.0f;
			for (auto const& [id, resource] : mTraversalResources.entries())
			{
				(void)id;
				if (resource->mLift && resource->mLiftSector.value == (uint64_t)plan.sectorIndex + 1)
				{ oldPosition = resource->mLiftPosition; break; }
			}
			auto nearest = min_element(found->values.begin(), found->values.end(), [&](auto a, auto b)
			{
				auto da = abs((float)(plan.y + a) - oldPosition);
				auto db = abs((float)(plan.y + b) - oldPosition);
				return da == db ? a < b : da < db;
			});
			found->g = (uint32_t)distance(found->values.begin(), nearest);
		}
		records = canonicalConstructionRecords(std::move(records));
		try
		{
			Building candidate(mName, mCellsWide, mDecksHigh);
			candidate.mDeserializingConstruction = true;
			for (auto const& record : records) candidate.applyConstructionRecord(record);
			candidate.finishBuild();
		}
		catch (Exception const& error) { diagnostic = error.getMessage(); return false; }
		catch (exception const& error) { diagnostic = error.what(); return false; }
		return true;
	}

	void Building::rebuildFromConstructionRecords(vector<ConstructionRecord> records)
	{
		struct SavedAgent { AgentId id; string name; uint32_t flags; uint32_t layer; Vector2 position; };
		vector<SavedAgent> agents;
		for (auto const& [id, agent] : mAgents.entries())
			agents.push_back({ id, agent->getName(), agent->getFlags(),
				agent->getSector()->getLayerIndex(), agent->getGlobalPosition() });
		resetForDeserialization(mName, mCellsWide, mDecksHigh);
		mDeserializingConstruction = true;
		try
		{
			for (auto const& record : records) applyConstructionRecord(record);
			finishBuild();
		}
		catch (...) { mDeserializingConstruction = false; throw; }
		mDeserializingConstruction = false;
		mConstructionRecords = std::move(records);
		mSimulationPaused = true;
		modify();
		for (auto const& saved : agents)
		{
			auto sector = getSectorAtPosition(saved.layer, saved.position.x, saved.position.y);
			if (!sector) continue;
			auto agent = make_unique<Agent>(saved.name);
			agent->setFlags(saved.flags);
			auto* raw = agent.get();
			raw->attachToBuilding(this);
			raw->mPosition = SectorPosition(sector.get(), saved.position - sector->getPosition());
			_getSector(sector->getIndex())->mAgents.insert(raw);
			mAgents.restore(saved.id, std::move(agent));
			mAgentIds.emplace(raw, saved.id);
		}
	}

	Building::LiftEditPlan Building::planResizeLift(uint32_t sectorIndex,
		uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const
	{
		LiftEditPlan plan;
		plan.sectorIndex = sectorIndex; plan.x = x; plan.y = y;
		plan.cellsWide = cellsWide; plan.decksHigh = decksHigh;
		if (sectorIndex >= mSectors.size() || !dynamic_pointer_cast<const LiftTransit>(mSectors[sectorIndex]))
		{ plan.diagnostic = "Only enclosed Lifts can be resized"; return plan; }
		auto lift = dynamic_pointer_cast<const LiftTransit>(mSectors[sectorIndex]);
		plan.move = x != lift->getCellX() || y != lift->getCellY();
		if (cellsWide < 1 || cellsWide > 2)
		{ plan.diagnostic = "A Lift must be one or two cells wide"; return plan; }
		if (decksHigh == 0 || x + cellsWide > mCellsWide || y + decksHigh > mDecksHigh)
		{ plan.diagnostic = "The Lift shaft is outside the Building bounds"; return plan; }
		if (!lift->getAgents().empty())
		{ plan.diagnostic = "The Lift cannot be edited while agents occupy it"; return plan; }
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			if (!resource->mLift || resource->mLiftSector.value != (uint64_t)sectorIndex + 1) continue;
			bool active = !resource->mAdmissionQueue.empty() || !resource->mLiftConfirmationQueue.empty()
				|| !resource->mLiftTripIntents.empty() || resource->mLiftMoving
				|| any_of(resource->mOccupants.begin(), resource->mOccupants.end(), [](auto owner) { return (bool)owner; })
				|| any_of(resource->mAdmissionReservations.begin(), resource->mAdmissionReservations.end(),
					[](auto owner) { return (bool)owner; });
			for (auto const& [landingId, landing] : mTraversalResources.entries())
			{
				(void)landingId;
				if (landing->mLiftCoordinator != id) continue;
				active = active || !landing->mOpenLeases.empty()
					|| any_of(landing->mCrossingOwners.begin(), landing->mCrossingOwners.end(),
						[](auto owner) { return (bool)owner; });
				for (auto const& lane : landing->mQueueLanes) active = active || !lane.queue.empty();
			}
			if (active)
			{ plan.diagnostic = "The Lift cannot be edited while it has active journeys, queues, or reservations"; return plan; }
		}
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto occupant = mLayers[CORE_LAYER_BACK]->getCellDefinition(ix, iy).sectorIndex;
				if (occupant != ~0u && occupant != sectorIndex)
				{ plan.diagnostic = format("Sector at {},{} blocks the Lift", ix, iy); return plan; }
			}
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			auto const& first = mLayers[CORE_LAYER_FORE]->getCellDefinition(x, iy);
			if (first.sectorIndex == ~0u) continue;
			auto corridor = dynamic_pointer_cast<const Location>(mSectors[first.sectorIndex]);
			if (!corridor || !corridor->isCorridor()) continue;
			bool complete = true;
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cell = mLayers[CORE_LAYER_FORE]->getCellDefinition(ix, iy);
				complete = complete && cell.sectorIndex == first.sectorIndex && cell.isTraversableOnFoot();
				if (cell.hasObject())
				{
					auto owner = mSectors[cell.sectorIndex]->getObject(cell.sectorObjectIndex);
					uint32_t ownerLift;
					complete = complete && isLiftOwnedDoor(owner, &ownerLift) && ownerLift == sectorIndex;
				}
				complete = complete && cell.markers.empty();
			}
			if (complete) plan.stopOffsets.push_back(iy - y);
		}
		if (plan.stopOffsets.size() < 2)
		{ plan.diagnostic = "The Lift requires at least two fully overlapping Fore-layer corridor floors"; return plan; }
		vector<uint32_t> oldStops;
		for (uint32_t i = 0; i < lift->getNumStops(); ++i)
			oldStops.push_back((uint32_t)((int)lift->getStop(i).sector->getCellY() + lift->getStop(i).sectorOffsetY));
		vector<uint32_t> newStops;
		for (auto offset : plan.stopOffsets) newStops.push_back(y + offset);
		if (x != lift->getCellX() || y != lift->getCellY())
			plan.consequences.push_back("Move the Lift and rebuild every landing door and button");
		if (cellsWide != lift->getCellsWide())
			plan.consequences.push_back("Change the Lift width and rebuild every landing door and button");
		if (decksHigh < lift->getDecksHigh())
			plan.consequences.push_back("Shrink the Lift shaft");
		for (auto floor : oldStops) if (find(newStops.begin(), newStops.end(), floor) == newStops.end())
			plan.consequences.push_back(format("Remove Lift stop and landing at floor {}", floor));
		bool const destructive = !plan.consequences.empty();
		if (destructive)
			for (auto floor : newStops) if (find(oldStops.begin(), oldStops.end(), floor) == oldStops.end())
				plan.consequences.push_back(format("Create Lift stop and landing at floor {}", floor));
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			(void)id;
			if (!resource->mLift || resource->mLiftSector.value != (uint64_t)sectorIndex + 1) continue;
			auto currentFloor = (uint32_t)round(resource->mLiftPosition);
			if (find(newStops.begin(), newStops.end(), currentFloor) == newStops.end())
				plan.consequences.push_back("Relocate the Lift car to the nearest remaining stop");
		}
		vector<ConstructionRecord> records;
		plan.valid = prepareLiftEdit(plan, records, plan.diagnostic);
		return plan;
	}

	Building::LiftEditPlan Building::planRemoveLift(uint32_t sectorIndex) const
	{
		if (sectorIndex >= mSectors.size() || !dynamic_pointer_cast<const LiftTransit>(mSectors[sectorIndex]))
		{ LiftEditPlan plan; plan.diagnostic = "Only an enclosed Lift can be deleted"; return plan; }
		auto lift = dynamic_pointer_cast<const LiftTransit>(mSectors[sectorIndex]);
		auto plan = planResizeLift(sectorIndex, lift->getCellX(), lift->getCellY(),
			lift->getCellsWide(), lift->getDecksHigh());
		if (!plan.valid) return plan;
		plan.remove = true;
		plan.consequences.clear();
		for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
			plan.consequences.push_back(format("Delete Lift landing and stop {}", stop));
		vector<ConstructionRecord> records;
		plan.valid = prepareLiftEdit(plan, records, plan.diagnostic);
		return plan;
	}

	Building::LiftEditPlan Building::planRemoveLiftStop(uint32_t sectorIndex, uint32_t stopIndex) const
	{
		LiftEditPlan invalid;
		if (sectorIndex >= mSectors.size())
		{ invalid.diagnostic = "The selected Lift no longer exists"; return invalid; }
		auto lift = dynamic_pointer_cast<const LiftTransit>(mSectors[sectorIndex]);
		if (!lift || stopIndex >= lift->getNumStops())
		{ invalid.diagnostic = "The selected Lift stop no longer exists"; return invalid; }
		if (lift->getNumStops() <= 2)
		{ invalid.diagnostic = "Deleting this landing would leave the Lift with fewer than two stops"; return invalid; }
		auto plan = planResizeLift(sectorIndex, lift->getCellX(), lift->getCellY(),
			lift->getCellsWide(), lift->getDecksHigh());
		if (!plan.valid) return plan;
		plan.stopOffsets.clear();
		uint32_t removedFloor = 0;
		for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
		{
			auto const& value = lift->getStop(stop);
			auto floor = (uint32_t)((int)value.sector->getCellY() + value.sectorOffsetY);
			if (stop == stopIndex) { removedFloor = floor; continue; }
			plan.stopOffsets.push_back(floor - lift->getCellY());
		}
		plan.consequences = { format("Delete Lift landing, button, pathing, and stop at floor {}", removedFloor) };
		vector<ConstructionRecord> records;
		plan.valid = prepareLiftEdit(plan, records, plan.diagnostic);
		return plan;
	}

	uint32_t Building::applyLiftEdit(LiftEditPlan const& requested)
	{
		if (!mSimulationPaused) throw BuildingException(this, "Editing a Lift requires the simulation to be paused");
		auto plan = requested.remove ? planRemoveLift(requested.sectorIndex)
			: planResizeLift(requested.sectorIndex, requested.x, requested.y,
				requested.cellsWide, requested.decksHigh);
		if (!plan.valid) throw BuildingException(this, plan.diagnostic);
		if (!requested.remove && requested.stopOffsets.size() >= 2
			&& requested.stopOffsets != plan.stopOffsets)
		{
			plan.stopOffsets = requested.stopOffsets;
			plan.consequences = requested.consequences;
			vector<ConstructionRecord> validationRecords;
			if (!prepareLiftEdit(plan, validationRecords, plan.diagnostic))
				throw BuildingException(this, plan.diagnostic);
		}
		if (!plan.valid) throw BuildingException(this, plan.diagnostic);
		vector<ConstructionRecord> records; string diagnostic;
		if (!prepareLiftEdit(plan, records, diagnostic)) throw BuildingException(this, diagnostic);
		rebuildFromConstructionRecords(std::move(records));
		if (plan.remove) return ~0u;
		auto const& cell = mLayers[CORE_LAYER_BACK]->getCellDefinition(plan.x, plan.y);
		return cell.sectorIndex;
	}

	bool Building::prepareLocationEdit(LocationEditPlan const& plan,
		vector<ConstructionRecord>& records, uint32_t& newSectorIndex,
		string& diagnostic) const
	{
		auto createsSector = [](ConstructionType type)
		{
			return type == ConstructionType::Corridor || type == ConstructionType::Room
				|| type == ConstructionType::Ladder || type == ConstructionType::Staircase
				|| type == ConstructionType::Lift || type == ConstructionType::Shuttle;
		};
		auto referencesSector = [](ConstructionType type)
		{
			return type == ConstructionType::LightSwitch || type == ConstructionType::ForceBridge
				|| type == ConstructionType::SectorLadder || type == ConstructionType::PlatformLift
				|| type == ConstructionType::Walkway || type == ConstructionType::Marker
				|| type == ConstructionType::RemoveWall || type == ConstructionType::RemoveMarker
				|| type == ConstructionType::ObjectTombstone;
		};

		records.clear();
		newSectorIndex = ~0u;
		Building candidate(mName, mCellsWide, mDecksHigh);
		candidate.mDeserializingConstruction = true;
		vector<uint32_t> sectorMap(mSectors.size(), ~0u);
		uint32_t oldSectorIndex = 0;

		auto locationAt = [&](uint32_t x, uint32_t y) -> shared_ptr<const Sector>
		{
			if (x >= candidate.mCellsWide || y >= candidate.mDecksHigh) return nullptr;
			auto const& cell = candidate.mLayers[CORE_LAYER_FORE]->getCellDefinition(x, y);
			if (cell.sectorIndex == ~0u) return nullptr;
			auto sector = candidate.getSector(cell.sectorIndex);
			return sector && sector->getType() == SectorType::Location ? sector : nullptr;
		};

		try
		{
			for (auto source : mConstructionRecords)
			{
				auto const producer = createsSector(source.type);
				auto const sourceSectorIndex = producer ? oldSectorIndex++ : ~0u;
				if (producer && sourceSectorIndex == plan.sectorIndex)
				{
					if (plan.remove) continue;
					if (source.type == ConstructionType::Corridor)
					{
						source.a = plan.y; source.b = plan.x;
						source.c = plan.cellsWide; source.d = plan.decksHigh;
					}
					else if (source.type == ConstructionType::Room)
					{
						source.b = plan.y; source.c = plan.x;
						source.d = plan.cellsWide; source.e = plan.decksHigh;
					}
					else
					{
						diagnostic = "Only rooms and corridors can be resized";
						return false;
					}
				}

				if (referencesSector(source.type))
				{
					if (source.a >= sectorMap.size() || sectorMap[source.a] == ~0u)
						continue; // The owning Location, and therefore this object, was deleted.
					source.a = sectorMap[source.a];
				}

				if (source.type == ConstructionType::Lift)
				{
					auto corridorAt = [&](uint32_t x, uint32_t y) -> shared_ptr<const Sector>
					{
						auto sector = locationAt(x, y);
						auto location = dynamic_pointer_cast<const Location>(sector);
						return location && location->isCorridor() ? sector : nullptr;
					};
					for (auto const& [id, resource] : mTraversalResources.entries())
					{
						(void)id;
						if (resource->mLift && resource->mLiftSector.value == (uint64_t)sourceSectorIndex + 1)
						{
							source.g = resource->mLiftCurrentStop;
							break;
						}
					}
					auto originalStops = source.values;
					vector<uint32_t> stops;
					for (auto offset : originalStops)
					{
						auto y = source.a + offset;
						auto first = corridorAt(source.b, y);
						bool supported = first != nullptr;
						for (uint32_t x = source.b; supported && x < source.b + source.c; ++x)
							supported = corridorAt(x, y) == first;
						if (supported) stops.push_back(offset);
					}
					if (stops.size() < 2)
					{
						diagnostic = "The edit would leave a Lift with fewer than two stops. "
							"Delete the Lift first (transit deletion is not yet supported by the editor).";
						return false;
					}
					uint32_t currentOffset = source.g < originalStops.size()
						? originalStops[source.g] : originalStops.front();
					auto nearest = min_element(stops.begin(), stops.end(), [currentOffset](auto a, auto b)
					{
						auto da = abs((int64_t)a - (int64_t)currentOffset);
						auto db = abs((int64_t)b - (int64_t)currentOffset);
						return da == db ? a < b : da < db;
					});
					source.g = (uint32_t)distance(stops.begin(), nearest);
					source.values = std::move(stops);
				}
				else if (source.type == ConstructionType::Shuttle)
				{
					for (auto const& [id, resource] : mTraversalResources.entries())
					{
						(void)id;
						if (resource->mShuttle && resource->mLiftSector.value == (uint64_t)sourceSectorIndex + 1)
						{
							source.f = resource->mLiftCurrentStop;
							break;
						}
					}
					auto originalStops = source.values;
					vector<uint32_t> stops;
					for (auto offset : originalStops)
					{
						bool supported = false;
						for (uint32_t car = 0; car < source.d; ++car)
						{
							auto doorX = source.b + offset + car * (source.e + 1) + 1;
							supported = supported || locationAt(doorX, source.a) != nullptr;
						}
						if (supported) stops.push_back(offset);
					}
					if (stops.size() < 2)
					{
						diagnostic = "The edit would leave a Shuttle with fewer than two stops. "
							"Delete the Shuttle first (transit deletion is not yet supported by the editor).";
						return false;
					}
					uint32_t currentOffset = source.f < originalStops.size()
						? originalStops[source.f] : originalStops.front();
					auto nearest = min_element(stops.begin(), stops.end(), [currentOffset](auto a, auto b)
					{
						auto da = abs((int64_t)a - (int64_t)currentOffset);
						auto db = abs((int64_t)b - (int64_t)currentOffset);
						return da == db ? a < b : da < db;
					});
					source.f = (uint32_t)distance(stops.begin(), nearest);
					source.p = true;
					source.values = std::move(stops);
				}
				else if (source.type == ConstructionType::Staircase)
				{
					vector<uint32_t> supported;
					for (uint32_t y = source.a; y < source.a + source.c; ++y)
					{
						auto first = locationAt(source.b, y);
						if (first && locationAt(source.b + 1, y) == first) supported.push_back(y);
					}
					if (supported.size() < 2)
					{
						diagnostic = "The edit would leave a Staircase with fewer than two supported decks. "
							"Delete the Staircase first (transit deletion is not yet supported by the editor).";
						return false;
					}
					for (size_t i = 1; i < supported.size(); ++i)
						if (supported[i] != supported[i - 1] + 1)
						{
							diagnostic = "The edit would create an unsupported gap in a Staircase.";
							return false;
						}
					source.a = supported.front();
					source.c = (uint32_t)supported.size();
				}

				auto const before = (uint32_t)candidate.mSectors.size();
				try
				{
					candidate.applyConstructionRecord(source);
				}
				catch (Exception const& error)
				{
					if (producer)
					{
						diagnostic = error.getMessage();
						if (source.type == ConstructionType::Ladder)
							diagnostic += " Delete the Ladder first (transit deletion is not yet supported by the editor).";
						return false;
					}
					if (source.type == ConstructionType::Marker)
					{
						// Preserve authored object indices so a later RemoveMarker command
						// cannot accidentally remove a different Marker after this one is cropped.
						ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
						tombstone.a = source.a;
						candidate.applyConstructionRecord(tombstone);
						records.push_back(std::move(tombstone));
					}
					continue; // An object made invalid by the edit is part of the cascade.
				}
				if (producer)
				{
					sectorMap[sourceSectorIndex] = before;
					if (sourceSectorIndex == plan.sectorIndex) newSectorIndex = before;
				}
				records.push_back(std::move(source));
			}
			candidate.finishBuild();
		}
		catch (Exception const& error)
		{
			diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			diagnostic = error.what();
			return false;
		}
		return true;
	}

	bool Building::prepareObjectMove(ObjectMovePlan const& plan,
		vector<ConstructionRecord>& records, uint32_t& newSectorIndex,
		uint32_t& newObjectIndex, string& diagnostic) const
	{
		records = mConstructionRecords;
		newSectorIndex = newObjectIndex = ~0u;
		if (plan.sectorIndex >= mSectors.size() || !mSectors[plan.sectorIndex]
			|| plan.objectIndex >= mSectors[plan.sectorIndex]->getNumObjects())
		{
			diagnostic = "The selected object no longer exists";
			return false;
		}
		auto object = mSectors[plan.sectorIndex]->getObject(plan.objectIndex);
		if (!object)
		{
			diagnostic = "The selected object no longer exists";
			return false;
		}

		auto owner = object->getSector();
		auto sourceX = object->getCellX();
		auto sourceY = object->getCellY();
		auto matches = [&](ConstructionRecord const& record)
		{
			switch (object->getObjectType())
			{
			case SectorObjectType::Door:
				return record.type == ConstructionType::Door
					&& record.b == sourceX && record.a == sourceY;
			case SectorObjectType::Window:
				return record.type == ConstructionType::Window
					&& record.c == sourceX && record.b == sourceY
					&& record.a == owner->getLayerIndex();
			case SectorObjectType::ForceBridge:
				return record.type == ConstructionType::ForceBridge && record.a == plan.sectorIndex
					&& owner->getCellX() + record.c == sourceX
					&& owner->getCellY() + record.b == sourceY;
			case SectorObjectType::Ladder:
				return record.type == ConstructionType::SectorLadder && record.a == plan.sectorIndex
					&& owner->getCellX() + record.c == sourceX
					&& owner->getCellY() + record.b == sourceY;
			case SectorObjectType::Lift:
				return record.type == ConstructionType::PlatformLift && record.a == plan.sectorIndex
					&& owner->getCellX() + record.c == sourceX
					&& owner->getCellY() + record.b == sourceY;
			case SectorObjectType::Walkway:
				return record.type == ConstructionType::Walkway && record.a == plan.sectorIndex
					&& owner->getCellX() + record.c == sourceX
					&& owner->getCellY() + record.b == sourceY;
			case SectorObjectType::Marker:
			{
				auto marker = static_pointer_cast<MarkerSectorObject>(object)->getMarker();
				return record.type == ConstructionType::Marker && record.a == plan.sectorIndex
					&& owner->getCellX() + (uint32_t)floor(record.x) == marker->getCellX()
					&& owner->getCellY() + record.b == marker->getCellY()
					&& fabs(record.x - floor(record.x) - marker->getOffset()) <= 0.001f;
			}
			default:
				return false;
			}
		};

		auto found = find_if(records.begin(), records.end(), matches);
		if (found == records.end())
		{
			diagnostic = "This object cannot be moved independently";
			return false;
		}

		auto const type = object->getObjectType();
		bool const pastePlaced = type == SectorObjectType::Door
			|| type == SectorObjectType::Window || type == SectorObjectType::Marker;
		auto targetOwner = getSectorAtPosition(owner->getLayerIndex(),
			(float)plan.x + 0.5f, (float)plan.y + 0.5f);
		if (pastePlaced && !targetOwner)
		{
			diagnostic = type == SectorObjectType::Marker
				? "Markers require a viable sector" : "The destination is outside a viable sector";
			return false;
		}

		auto targetRight = (uint64_t)plan.x + (uint32_t)ceil(object->getSize().x);
		auto targetTop = (uint64_t)plan.y + (uint32_t)ceil(object->getSize().y);
		if (targetRight > mCellsWide || targetTop > mDecksHigh)
		{
			diagnostic = "The destination is outside the building";
			return false;
		}
		if (!pastePlaced && (plan.x < owner->getCellX() || plan.y < owner->getCellY()
			|| targetRight > (uint64_t)owner->getCellX() + owner->getCellsWide()
			|| targetTop > (uint64_t)owner->getCellY() + owner->getDecksHigh()))
		{
			diagnostic = "The object must remain inside its sector";
			return false;
		}
		if (type == SectorObjectType::Door)
		{
			for (uint32_t layer = 0; layer < CORE_NUM_LAYERS; ++layer)
				for (uint32_t ix = plan.x; ix < targetRight; ++ix)
				{
					auto const& cell = mLayers[layer]->getCellDefinition(ix, plan.y);
					bool const selectedDoorOccupiesCell = plan.y == sourceY
						&& ix >= sourceX && ix < sourceX + object->getSize().x;
					if (!cell.markers.empty() || (cell.hasObject() && !selectedDoorOccupiesCell))
					{
						diagnostic = "Another object blocks the Door's destination";
						return false;
					}
				}
		}

		switch (type)
		{
		case SectorObjectType::Door: found->a = plan.y; found->b = plan.x; break;
		case SectorObjectType::Window: found->b = plan.y; found->c = plan.x; break;
		case SectorObjectType::ForceBridge:
		case SectorObjectType::Ladder:
		case SectorObjectType::Lift:
		case SectorObjectType::Walkway:
			found->b = plan.y - owner->getCellY();
			found->c = plan.x - owner->getCellX();
			break;
		case SectorObjectType::Marker:
			found->a = targetOwner->getIndex();
			found->b = plan.y - targetOwner->getCellY();
			found->x = (float)(plan.x - targetOwner->getCellX()) + 0.5f;
			break;
		default: break;
		}

		// Paste-placed objects may change owners. Replacing their old authored slot
		// with tombstones and appending the moved definition preserves all existing
		// object indices, just as cutting and pasting does.
		if (pastePlaced)
		{
			auto moved = *found;
			vector<ConstructionRecord> tombstones;
			set<uint32_t> owners;
			if (type == SectorObjectType::Door)
			{
				auto door = static_pointer_cast<DoorSectorObject>(object)->getDoor();
				for (uint32_t layer = 0; layer < CORE_NUM_LAYERS; ++layer)
				{
					auto sector = door->getSector(layer);
					if (!sector) continue;
					owners.insert(sector->getIndex());
					ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
					tombstone.a = sector->getIndex();
					tombstones.push_back(tombstone);
					bool hasControl = layer == CORE_LAYER_FORE ? moved.p : moved.q;
					if (hasControl) tombstones.push_back(tombstone);
				}
			}
			else if (type == SectorObjectType::Window)
			{
				auto window = static_pointer_cast<WindowSectorObject>(object)->getWindow();
				for (uint32_t layer = 0; layer < CORE_NUM_LAYERS; ++layer)
					if (auto sector = window->getSector(layer)) owners.insert(sector->getIndex());
				for (auto index : owners)
				{
					ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
					tombstone.a = index;
					tombstones.push_back(tombstone);
				}
			}
			else
			{
				ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
				tombstone.a = owner->getIndex();
				tombstones.push_back(tombstone);
			}
			auto position = (size_t)distance(records.begin(), found);
			records.erase(records.begin() + position);
			records.insert(records.begin() + position, tombstones.begin(), tombstones.end());
			records.push_back(std::move(moved));
			found = prev(records.end());
			newSectorIndex = targetOwner->getIndex();
		}
		else newSectorIndex = owner->getIndex();

		Building candidate(mName, mCellsWide, mDecksHigh);
		candidate.mDeserializingConstruction = true;
		try
		{
			for (auto const& record : records)
			{
				auto before = newSectorIndex < candidate.mSectors.size()
					? candidate.mSectors[newSectorIndex]->getNumObjects() : 0;
				candidate.applyConstructionRecord(record);
				if (&record == &*found) newObjectIndex = before;
			}
			candidate.finishBuild();
		}
		catch (Exception const& error)
		{
			diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			diagnostic = error.what();
			return false;
		}
		return true;
	}

	bool Building::removeSectorDoor(uint32_t sectorIndex, uint32_t objectIndex)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Deleting a Door requires the simulation to be paused");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto object = dynamic_pointer_cast<DoorSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) return false;

		uint32_t liftIndex, stopIndex;
		if (isLiftOwnedDoor(object, &liftIndex, &stopIndex))
		{
			auto lift = dynamic_pointer_cast<const LiftTransit>(mSectors[liftIndex]);
			if (!lift || lift->getNumStops() <= 2)
				throw BuildingException(this, "Deleting this landing would leave the Lift with fewer than two stops");
			auto plan = planResizeLift(liftIndex, lift->getCellX(), lift->getCellY(),
				lift->getCellsWide(), lift->getDecksHigh());
			if (!plan.valid) throw BuildingException(this, plan.diagnostic);
			plan.stopOffsets.clear();
			for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
			{
				if (stop == stopIndex) continue;
				auto const& value = lift->getStop(stop);
				plan.stopOffsets.push_back((uint32_t)((int)value.sector->getCellY()
					+ value.sectorOffsetY - (int)lift->getCellY()));
			}
			vector<ConstructionRecord> records; string diagnostic;
			if (!prepareLiftEdit(plan, records, diagnostic)) throw BuildingException(this, diagnostic);
			rebuildFromConstructionRecords(std::move(records));
			return true;
		}

		auto door = object->getDoor();
		auto source = find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door
					&& record.a == object->getCellY() && record.b == object->getCellX()
					&& record.c == door->getCellsWide();
			});
		if (source == mConstructionRecords.end()) return false;

		vector<ConstructionRecord> records;
		records.reserve(mConstructionRecords.size() + 3);
		for (auto const& record : mConstructionRecords)
		{
			if (&record != &*source)
			{
				records.push_back(record);
				continue;
			}
			// The Door is shared by both Locations and may also have added one
			// control to either Location. Preserve each Sector's authored indices.
			for (uint32_t layer = 0; layer < CORE_NUM_LAYERS; ++layer)
			{
				auto sector = door->getSector(layer);
				if (!sector) continue;
				ConstructionRecord doorTombstone{ ConstructionType::ObjectTombstone };
				doorTombstone.a = sector->getIndex();
				records.push_back(std::move(doorTombstone));
				bool hadControl = layer == CORE_LAYER_FORE ? source->p : source->q;
				if (hadControl)
				{
					ConstructionRecord controlTombstone{ ConstructionType::ObjectTombstone };
					controlTombstone.a = sector->getIndex();
					records.push_back(std::move(controlTombstone));
				}
			}
		}

		struct SavedAgent
		{
			AgentId id;
			string name;
			uint32_t flags;
			uint32_t layer;
			Vector2 position;
		};
		vector<SavedAgent> agents;
		for (auto const& [id, agent] : mAgents.entries())
			agents.push_back({ id, agent->getName(), agent->getFlags(),
				agent->getSector()->getLayerIndex(), agent->getGlobalPosition() });

		resetForDeserialization(mName, mCellsWide, mDecksHigh);
		mDeserializingConstruction = true;
		try
		{
			for (auto const& record : records) applyConstructionRecord(record);
			finishBuild();
		}
		catch (...)
		{
			mDeserializingConstruction = false;
			throw;
		}
		mDeserializingConstruction = false;
		mConstructionRecords = std::move(records);
		mSimulationPaused = true;
		modify();
		for (auto const& saved : agents)
		{
			auto sector = getSectorAtPosition(saved.layer, saved.position.x, saved.position.y);
			if (!sector) continue;
			auto agent = make_unique<Agent>(saved.name);
			agent->setFlags(saved.flags);
			auto* raw = agent.get();
			raw->attachToBuilding(this);
			raw->mPosition = SectorPosition(sector.get(), saved.position - sector->getPosition());
			_getSector(sector->getIndex())->mAgents.insert(raw);
			mAgents.restore(saved.id, std::move(agent));
			mAgentIds.emplace(raw, saved.id);
		}
		return true;
	}

	bool Building::removeSectorWindow(uint32_t sectorIndex, uint32_t objectIndex)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Deleting a Window requires the simulation to be paused");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto object = dynamic_pointer_cast<WindowSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) return false;

		auto window = object->getWindow();
		auto sourceX = object->getCellX();
		auto sourceY = object->getCellY();
		auto sourceLayer = object->getSector()->getLayerIndex();
		auto matches = [&](ConstructionRecord const& record)
		{
			return record.type == ConstructionType::Window && record.a == sourceLayer
				&& record.b == sourceY && record.c == sourceX
				&& record.d == window->getCellsWide() && record.e == window->getDecksHigh();
		};
		auto source = find_if(mConstructionRecords.begin(), mConstructionRecords.end(), matches);
		if (source == mConstructionRecords.end()) return false;

		vector<ConstructionRecord> records;
		records.reserve(mConstructionRecords.size() + 1);
		for (auto const& record : mConstructionRecords)
		{
			if (&record != &*source)
			{
				records.push_back(record);
				continue;
			}
			// Keep later authored object indices stable in every Sector that shared
			// the Window, while omitting the Window and its traversal resource.
			set<uint32_t> sectorIndices;
			for (uint32_t layer = 0; layer < CORE_NUM_LAYERS; ++layer)
				if (auto sector = window->getSector(layer)) sectorIndices.insert(sector->getIndex());
			for (auto index : sectorIndices)
			{
				ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
				tombstone.a = index;
				records.push_back(std::move(tombstone));
			}
		}

		struct SavedAgent
		{
			AgentId id;
			string name;
			uint32_t flags;
			uint32_t layer;
			Vector2 position;
		};
		vector<SavedAgent> agents;
		for (auto const& [id, agent] : mAgents.entries())
			agents.push_back({ id, agent->getName(), agent->getFlags(),
				agent->getSector()->getLayerIndex(), agent->getGlobalPosition() });

		resetForDeserialization(mName, mCellsWide, mDecksHigh);
		mDeserializingConstruction = true;
		try
		{
			for (auto const& record : records) applyConstructionRecord(record);
			finishBuild();
		}
		catch (...)
		{
			mDeserializingConstruction = false;
			throw;
		}
		mDeserializingConstruction = false;
		mConstructionRecords = std::move(records);
		mSimulationPaused = true;
		modify();
		for (auto const& saved : agents)
		{
			auto sector = getSectorAtPosition(saved.layer, saved.position.x, saved.position.y);
			if (!sector) continue;
			auto agent = make_unique<Agent>(saved.name);
			agent->setFlags(saved.flags);
			auto* raw = agent.get();
			raw->attachToBuilding(this);
			raw->mPosition = SectorPosition(sector.get(), saved.position - sector->getPosition());
			_getSector(sector->getIndex())->mAgents.insert(raw);
			mAgents.restore(saved.id, std::move(agent));
			mAgentIds.emplace(raw, saved.id);
		}
		return true;
	}

	Building::ObjectMovePlan Building::planMoveSectorObject(uint32_t sectorIndex,
		uint32_t objectIndex, uint32_t x, uint32_t y) const
	{
		ObjectMovePlan plan;
		plan.sectorIndex = sectorIndex;
		plan.objectIndex = objectIndex;
		plan.x = x;
		plan.y = y;
		vector<ConstructionRecord> records;
		uint32_t ignoredSector, ignoredObject;
		plan.valid = prepareObjectMove(plan, records, ignoredSector, ignoredObject, plan.diagnostic);
		return plan;
	}

	shared_ptr<const SectorObject> Building::applyObjectMove(ObjectMovePlan const& requested)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Moving an object requires the simulation to be paused");
		auto plan = requested;
		vector<ConstructionRecord> records;
		uint32_t newSectorIndex, newObjectIndex;
		string diagnostic;
		if (!prepareObjectMove(plan, records, newSectorIndex, newObjectIndex, diagnostic))
			throw BuildingException(this, diagnostic);

		struct SavedAgent
		{
			AgentId id;
			string name;
			uint32_t flags;
			uint32_t layer;
			Vector2 position;
		};
		vector<SavedAgent> agents;
		for (auto const& [id, agent] : mAgents.entries())
			agents.push_back({ id, agent->getName(), agent->getFlags(),
				agent->getSector()->getLayerIndex(), agent->getGlobalPosition() });

		resetForDeserialization(mName, mCellsWide, mDecksHigh);
		mDeserializingConstruction = true;
		try
		{
			for (auto const& record : records) applyConstructionRecord(record);
			finishBuild();
		}
		catch (...)
		{
			mDeserializingConstruction = false;
			throw;
		}
		mDeserializingConstruction = false;
		mConstructionRecords = std::move(records);
		mSimulationPaused = true;
		modify();
		for (auto const& saved : agents)
		{
			auto sector = getSectorAtPosition(saved.layer, saved.position.x, saved.position.y);
			if (!sector) continue;
			auto agent = make_unique<Agent>(saved.name);
			agent->setFlags(saved.flags);
			auto* raw = agent.get();
			raw->attachToBuilding(this);
			raw->mPosition = SectorPosition(sector.get(), saved.position - sector->getPosition());
			_getSector(sector->getIndex())->mAgents.insert(raw);
			mAgents.restore(saved.id, std::move(agent));
			mAgentIds.emplace(raw, saved.id);
		}
		return getSector(newSectorIndex)->getObject(newObjectIndex);
	}

	Building::LocationEditPlan Building::planResizeLocation(uint32_t sectorIndex,
		uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const
	{
		LocationEditPlan plan;
		plan.sectorIndex = sectorIndex;
		plan.x = x; plan.y = y; plan.cellsWide = cellsWide; plan.decksHigh = decksHigh;
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| mSectors[sectorIndex]->getType() != SectorType::Location)
		{
			plan.diagnostic = "Only rooms and corridors can be resized";
			return plan;
		}
		auto sector = mSectors[sectorIndex];
		plan.move = (x != sector->getCellX() || y != sector->getCellY())
			&& cellsWide == sector->getCellsWide() && decksHigh == sector->getDecksHigh();
		if (cellsWide == 0 || decksHigh == 0 || x + cellsWide > mCellsWide || y + decksHigh > mDecksHigh)
		{
			plan.diagnostic = "The resized sector is outside the Building bounds";
			return plan;
		}
		bool corridor = sector->getTopDeckHeight() == CORE_CORRIDOR_HEIGHT;
		if (corridor && !plan.move
			&& (y != sector->getCellY() || decksHigh != sector->getDecksHigh()))
		{
			plan.diagnostic = "Corridors cannot be resized vertically";
			return plan;
		}
		auto layer = mLayers[sector->getLayerIndex()];
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto occupant = layer->getCellDefinition(ix, iy).sectorIndex;
				if (occupant != ~0u && occupant != sectorIndex)
				{
					plan.diagnostic = format("Sector at {},{} blocks the resize", ix, iy);
					return plan;
				}
			}

		set<void const*> seen;
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || !seen.insert(object.get()).second) continue;
			if (plan.move)
			{
				auto type = object->getObjectType();
				if (type == SectorObjectType::Door || type == SectorObjectType::Window
					|| type == SectorObjectType::BulkheadDoor)
					plan.consequences.push_back("Delete " + object->getDescription());
				continue;
			}
			Vector2 min, max;
			object->getBounds(min, max);
			bool inside = min.x >= x && max.x <= x + cellsWide
				&& min.y >= y && max.y <= y + decksHigh;
			bool losesFloor = y != sector->getCellY() && object->getCellY() == sector->getCellY();
			if (!inside || losesFloor) plan.consequences.push_back("Delete " + object->getDescription());
		}
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent->getSector() != sector.get()) continue;
			if (plan.move) continue;
			auto pos = agent->getGlobalPosition();
			if (pos.x < x || pos.x > x + cellsWide || pos.y < y || pos.y > y + decksHigh
				|| (y != sector->getCellY() && (uint32_t)floor(pos.y) == sector->getCellY()))
				plan.consequences.push_back("Delete Agent " + agent->getName());
		}
		for (auto const& candidate : mSectors)
		{
			auto transit = dynamic_pointer_cast<Transit const>(candidate);
			if (!transit) continue;
			for (uint32_t stop = 0; stop < transit->getNumStops(); ++stop)
			{
				auto const& transitStop = transit->getStop(stop);
				if (transitStop.sector != sector) continue;
				auto stopX = (int)sector->getCellX() + transitStop.sectorOffsetX;
				auto stopY = (int)sector->getCellY() + transitStop.sectorOffsetY;
				if (plan.move || stopX < (int)x || stopX >= (int)(x + cellsWide)
					|| stopY < (int)y || stopY >= (int)(y + decksHigh))
				{
					plan.consequences.push_back(format("Remove stop {} from {}", stop, transit->getName()));
					for (auto const& [id, resource] : mTraversalResources.entries())
					{
						(void)id;
						if (resource->mLiftSector.value == (uint64_t)transit->getIndex() + 1
							&& resource->mLiftCurrentStop == stop)
							plan.consequences.push_back("Relocate " + transit->getName() + " to the nearest remaining stop");
					}
				}
			}
		}
		vector<ConstructionRecord> records;
		uint32_t ignored;
		plan.valid = prepareLocationEdit(plan, records, ignored, plan.diagnostic);
		return plan;
	}

	Building::LocationEditPlan Building::planRemoveLocation(uint32_t sectorIndex) const
	{
		LocationEditPlan plan;
		plan.remove = true;
		plan.sectorIndex = sectorIndex;
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| mSectors[sectorIndex]->getType() != SectorType::Location)
		{
			plan.diagnostic = "Only rooms and corridors can be deleted";
			return plan;
		}
		auto sector = mSectors[sectorIndex];
		plan.x = sector->getCellX(); plan.y = sector->getCellY();
		plan.cellsWide = sector->getCellsWide(); plan.decksHigh = sector->getDecksHigh();
		set<void const*> seen;
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (object && seen.insert(object.get()).second)
				plan.consequences.push_back("Delete " + object->getDescription());
		}
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent->getSector() == sector.get())
				plan.consequences.push_back("Delete Agent " + agent->getName());
		}
		for (auto const& candidate : mSectors)
		{
			auto transit = dynamic_pointer_cast<Transit const>(candidate);
			if (!transit) continue;
			for (uint32_t stop = 0; stop < transit->getNumStops(); ++stop)
				if (transit->getStop(stop).sector == sector)
				{
					plan.consequences.push_back(format("Remove stop {} from {}", stop, transit->getName()));
					for (auto const& [id, resource] : mTraversalResources.entries())
					{
						(void)id;
						if (resource->mLiftSector.value == (uint64_t)transit->getIndex() + 1
							&& resource->mLiftCurrentStop == stop)
							plan.consequences.push_back("Relocate " + transit->getName() + " to the nearest remaining stop");
					}
				}
		}
		vector<ConstructionRecord> records;
		uint32_t ignored;
		plan.valid = prepareLocationEdit(plan, records, ignored, plan.diagnostic);
		return plan;
	}

	uint32_t Building::applyLocationEdit(LocationEditPlan const& requested)
	{
		LocationEditPlan plan = requested.remove
			? planRemoveLocation(requested.sectorIndex)
			: planResizeLocation(requested.sectorIndex, requested.x, requested.y,
				requested.cellsWide, requested.decksHigh);
		if (!plan.valid) throw BuildingException(this, plan.diagnostic);

		vector<ConstructionRecord> records;
		uint32_t newSectorIndex;
		string diagnostic;
		if (!prepareLocationEdit(plan, records, newSectorIndex, diagnostic))
			throw BuildingException(this, diagnostic);

		struct SavedAgent
		{
			AgentId id;
			string name;
			uint32_t flags;
			uint32_t layer;
			Vector2 position;
		};
		vector<SavedAgent> agents;
		for (auto const& [id, agent] : mAgents.entries())
		{
			auto position = agent->getGlobalPosition();
			if (plan.move && agent->getSector()->getIndex() == plan.sectorIndex)
			{
				position.x += (float)plan.x - (float)mSectors[plan.sectorIndex]->getCellX();
				position.y += (float)plan.y - (float)mSectors[plan.sectorIndex]->getCellY();
			}
			agents.push_back({ id, agent->getName(), agent->getFlags(),
				agent->getSector()->getLayerIndex(), position });
		}

		resetForDeserialization(mName, mCellsWide, mDecksHigh);
		mDeserializingConstruction = true;
		try
		{
			for (auto const& record : records) applyConstructionRecord(record);
			finishBuild();
		}
		catch (...)
		{
			mDeserializingConstruction = false;
			throw;
		}
		mDeserializingConstruction = false;
		mConstructionRecords = std::move(records);
		mSimulationPaused = true;
		modify();

		for (auto const& saved : agents)
		{
			auto sector = getSectorAtPosition(saved.layer, saved.position.x, saved.position.y);
			if (!sector) continue;
			auto cellX = (uint32_t)floor(saved.position.x);
			auto cellY = (uint32_t)floor(saved.position.y);
			if (cellX >= mCellsWide || cellY >= mDecksHigh) continue;
			if (sector->getType() == SectorType::Location
				&& !mLayers[saved.layer]->getCellDefinition(cellX, cellY).isTraversableOnFoot()) continue;
			auto agent = make_unique<Agent>(saved.name);
			agent->setFlags(saved.flags);
			auto* raw = agent.get();
			raw->attachToBuilding(this);
			raw->mPosition = SectorPosition(sector.get(), saved.position - sector->getPosition());
			_getSector(sector->getIndex())->mAgents.insert(raw);
			mAgents.restore(saved.id, std::move(agent));
			mAgentIds.emplace(raw, saved.id);
		}
		return newSectorIndex;
	}
}
