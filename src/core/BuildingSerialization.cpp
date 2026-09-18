#include "core/Building.h"
#include "core/SerializationException.h"
#include "core/Exceptions.h"
#include "core/Transit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/LadderTransit.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/StairwellTransit.h"
#include "core/StaircaseTransit.h"
#include "core/Location.h"
#include "core/DoorSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/MarkerSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/Button.h"
#include "core/WindowSectorObject.h"
#include "core/YamlSerializer.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <stdexcept>
#include <utility>

namespace core
{
	using namespace std;

	static bool walkwayHasOccupant(shared_ptr<const Sector> const& sector, uint32_t cellX,
		uint32_t cellY)
	{
		if (!sector) return false;
		for (auto const* agent : sector->getAgents())
		{
			if (!agent) continue;
			auto const position = agent->getGlobalPosition();
			if (position.x >= cellX && position.x < (float)cellX + 1.0f
				&& fabs(position.y - (float)cellY) <= 0.001f) return true;
		}
		return false;
	}

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
		case ConstructionType::Stairwell: return "stairwell";
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
			serializer.writeUint32("layer", record.layer);
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("cellsWide", record.c); serializer.writeUint32("decksHigh", record.d); break;
		case ConstructionType::Room:
			// Version 4 records the layer index directly instead of a fore/back name.
			serializer.writeString("name", record.name); serializer.writeUint32("layer", record.a);
			serializer.writeUint32("y", record.b); serializer.writeUint32("x", record.c);
			serializer.writeUint32("cellsWide", record.d); serializer.writeUint32("decksHigh", record.e);
			serializer.writeFloat("topDeckHeight", record.x); break;
		case ConstructionType::Ladder:
			serializer.writeUint32("layer", record.layer);
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("decksHigh", record.c); serializer.writeBool("extensible", record.p);
			serializer.writeBool("startExtended", record.q);
			serializer.writeUint32("directionalBatchLimit", record.d); break;
		case ConstructionType::Stairwell:
			serializer.writeUint32("layer", record.layer);
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("decksHigh", record.c); serializer.writeString("mountSide", sideName(record.i));
			serializer.writeUint32("directionalCapacity", record.d);
			serializer.writeUint32("directionalBatchLimit", record.e); break;
		case ConstructionType::Staircase:
			serializer.writeUint32("layer", record.layer);
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("cellsWide", record.c); serializer.writeString("riseSide", sideName(record.i));
			serializer.writeFloat("speed", record.x); break;
		case ConstructionType::Lift:
			serializer.writeUint32("layer", record.layer);
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("cellsWide", record.c); serializer.writeUint32("decksHigh", record.e);
			writeStops(); serializer.writeUint32("capacity", record.d);
			serializer.writeFloat("minimumDwellSeconds", record.x);
			serializer.writeFloat("maximumBoardingSeconds", record.y);
			serializer.writeUint32("initialStop", record.g); break;
		case ConstructionType::Shuttle:
			serializer.writeUint32("layer", record.layer);
			serializer.writeUint32("y", record.a); serializer.writeUint32("x", record.b);
			serializer.writeUint32("cellsWide", record.c); serializer.writeUint32("numCars", record.d);
			serializer.writeUint32("carWidth", record.e); writeStops();
			serializer.writeUint32("initialStop", record.f); serializer.writeUint32("capacityPerCarriage", record.g);
			serializer.writeUint32("doorMask", record.h ? record.h : (1u << 1));
			serializer.writeFloat("minimumDwellSeconds", record.x);
			serializer.writeFloat("maximumBoardingSeconds", record.y);
			serializer.writeBool("allowPartialLandings", record.p); break;
		case ConstructionType::Door:
			serializer.writeUint32("layer", record.layer);
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
			serializer.writeUint32("layer", record.a); serializer.writeUint32("y", record.b);
			serializer.writeUint32("x", record.c); serializer.writeUint32("cellsWide", record.d);
			serializer.writeUint32("decksHigh", record.e); serializer.writeBool("traversable", record.p);
			serializer.writeString("initialState", states[record.i]); serializer.writeString("style", styles[record.j]); break;
		}
		case ConstructionType::BulkheadDoor:
			serializer.writeUint32("layer", record.a); serializer.writeUint32("y", record.b);
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
			serializer.writeUint32("directionalBatchLimit", record.e); break;
		case ConstructionType::PlatformLift:
			serializer.writeUint32("sectorIndex", record.a); serializer.writeUint32("deckIndex", record.b);
			serializer.writeUint32("xOffset", record.c); serializer.writeUint32("cellsWide", record.d);
			writeStops(); serializer.writeUint32("capacity", record.e);
			serializer.writeFloat("stopDurationSeconds", record.z); break;
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
		serializer.writeUint32("version", 4);
		serializer.writeString("name", mName);
		serializer.writeUint32("cellsWide", mCellsWide);
		serializer.writeUint32("decksHigh", mDecksHigh);
		serializer.writeUint32("layers", getLayerCount());

		serializer.beginArray("layerNames");
		for (auto const& name : mLayerNames) serializer.writeString("", name);
		serializer.endArray();

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
			auto const& resetPosition = agent->mResetPosition.sector()
				? agent->mResetPosition : agent->mPosition;
			auto const* sector = resetPosition.sector();
			if (!sector)
			{
				throw SerializationException("Cannot serialize a Building-owned Agent without a Sector");
			}
			serializer.beginMap("");
			serializer.writeUint64("id", id.value);
			agent->serialize(serializer, workData);
			serializer.writeUint32("sector", sector->getIndex());
			serializer.writeFloat("localX", resetPosition.local().x);
			serializer.writeFloat("localY", resetPosition.local().y);
			if (agent->mResetPath && !agent->mResetPath->nodes.empty())
			{
				auto const& destination = agent->mResetPath->nodes.back().targetVertex;
				if (!destination || !destination->getSector())
				{
					throw SerializationException("Cannot serialize an Agent path without a destination Sector");
				}
				serializer.beginMap("path");
				serializer.writeUint32("destinationSector", destination->getSector()->getIndex());
				serializer.writeFloat("destinationLocalX", destination->getSectorOffset().x);
				serializer.writeFloat("destinationLocalY", destination->getSectorOffset().y);
				serializer.writeBool("active", agent->mResetPathActive);
				serializer.endMap();
			}
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

		auto readLayer = [&](char const* field) -> uint32_t
		{
			// Version 4 writes a layer index.  The first version 4 writer still used the
			// legacy "fore" / "back" names, so both spellings are accepted.
			auto const legacy = serializer.readString(field, true, "");
			if (legacy == "fore") return static_cast<uint32_t>(0);
			if (legacy == "back") return static_cast<uint32_t>(layerBehind(0));

			auto const layer = serializer.readUint32(field);
			if (layer >= static_cast<uint32_t>(mLayers.size()))
			{
				throw SerializationException(format("Layer {} is outside the Building's {} layers",
						layer, mLayers.size()));
			}
			return layer;
		};
		auto readLayerOr = [&](char const* field, uint32_t fallback) -> uint32_t
		{
			// Transits and Doors only began recording their own Layer with this ticket.
			// An older record replays against the fallback Layer it was written on.
			if (!serializer.hasField(field)) return fallback;
			return readLayer(field);
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
			record.layer = readLayerOr("layer", 0u);
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("cellsWide"); record.d = serializer.readUint32("decksHigh"); break;
		case ConstructionType::Room:
			record.name = serializer.readString("name"); record.a = readLayer("layer");
			record.b = serializer.readUint32("y"); record.c = serializer.readUint32("x");
			record.d = serializer.readUint32("cellsWide"); record.e = serializer.readUint32("decksHigh");
			record.x = serializer.readFloat("topDeckHeight"); break;
		case ConstructionType::Ladder:
			record.layer = readLayerOr("layer", layerBehind(0));
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("decksHigh"); record.p = serializer.readBool("extensible");
			record.q = serializer.readBool("startExtended");
			(void)serializer.readFloat("agentSpacing", true, CORE_LADDER_AGENT_SPACING);
			record.d = serializer.readUint32("directionalBatchLimit"); break;
		case ConstructionType::Stairwell:
			record.layer = readLayerOr("layer", layerBehind(0));
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("decksHigh"); record.i = readSide("mountSide");
			record.d = serializer.readUint32("directionalCapacity");
			record.e = serializer.readUint32("directionalBatchLimit"); break;
		case ConstructionType::Staircase:
			record.layer = readLayerOr("layer", layerBehind(0));
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("cellsWide"); record.i = readSide("riseSide");
			record.x = serializer.readFloat("speed", true, 0.0f); break;
		case ConstructionType::Lift:
			record.layer = readLayerOr("layer", layerBehind(0));
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("cellsWide"); record.e = serializer.readUint32("decksHigh");
			readStops(); record.d = serializer.readUint32("capacity");
			record.x = serializer.readFloat("minimumDwellSeconds");
			record.y = serializer.readFloat("maximumBoardingSeconds");
			record.g = serializer.readUint32("initialStop"); break;
		case ConstructionType::Shuttle:
			record.layer = readLayerOr("layer", layerBehind(0));
			record.a = serializer.readUint32("y"); record.b = serializer.readUint32("x");
			record.c = serializer.readUint32("cellsWide"); record.d = serializer.readUint32("numCars");
			record.e = serializer.readUint32("carWidth"); readStops();
			record.f = serializer.readUint32("initialStop"); record.g = serializer.readUint32("capacityPerCarriage");
			record.h = serializer.readUint32("doorMask", true, 1u << 1);
			record.x = serializer.readFloat("minimumDwellSeconds");
			record.y = serializer.readFloat("maximumBoardingSeconds");
			record.p = serializer.readBool("allowPartialLandings"); break;
		case ConstructionType::Door:
			record.layer = readLayerOr("layer", 0u);
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
			(void)serializer.readFloat("agentSpacing", true, CORE_LADDER_AGENT_SPACING);
			record.e = serializer.readUint32("directionalBatchLimit"); break;
		case ConstructionType::PlatformLift:
			record.a = serializer.readUint32("sectorIndex"); record.b = serializer.readUint32("deckIndex");
			record.c = serializer.readUint32("xOffset"); record.d = serializer.readUint32("cellsWide");
			readStops(); record.e = serializer.readUint32("capacity");
			// Legacy timing fields remain accepted for old maps, but PlatformLift now
			// has one independent per-stop duration.
			record.x = serializer.readFloat("minimumDwellSeconds", true, CORE_LIFT_DOOR_PAUSE_TIME);
			record.y = serializer.readFloat("maximumBoardingSeconds", true,
				CORE_PLATFORM_LIFT_STOP_DURATION);
			record.z = serializer.readFloat("stopDurationSeconds", true, record.y); break;
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
		if (version < 1 || version > 4)
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

		auto const layerCount = serializer.readUint32("layers", true, 2);
		if (layerCount < 2 || layerCount > CORE_MAX_LAYERS)
		{
			throw SerializationException(format("Building layer count {} is out of range", layerCount));
		}
		mLayers.resize(layerCount);
		mLayerNames.resize(layerCount);
		for (uint32_t i = 0; i < layerCount; ++i)
		{
			mLayerNames[i] = defaultLayerName(i);
		}
		if (serializer.hasField("layerNames"))
		{
			serializer.beginArray("layerNames");
			uint32_t index = 0;
			while (serializer.nextArrayItem())
			{
				if (index >= layerCount)
				{
					throw SerializationException("layerNames array is longer than the layer count");
				}
				mLayerNames[index++] = serializer.readString("");
			}
			serializer.endArray();
			if (index != layerCount)
			{
				throw SerializationException("layerNames array is shorter than the layer count");
			}
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
		string ladderDiagnostic;
		if (!normalizeRoomLadderRecords(records, ladderDiagnostic))
			throw SerializationException(ladderDiagnostic);

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
			optional<uint32_t> destinationSectorIndex;
			float destinationLocalX{ 0.0f };
			float destinationLocalY{ 0.0f };
			bool pathActive{ false };
			if (serializer.hasField("path"))
			{
				serializer.beginMap("path");
				destinationSectorIndex = serializer.readUint32("destinationSector");
				destinationLocalX = serializer.readFloat("destinationLocalX");
				destinationLocalY = serializer.readFloat("destinationLocalY");
				pathActive = serializer.readBool("active");
				serializer.endMap();
			}
			serializer.endMap();

			if (mAgents.find(id))
			{
				throw SerializationException("Serialized Agent IDs must be unique");
			}
			auto sector = _getSector(sectorIndex);
			auto* rawAgent = agent.get();
			rawAgent->attachToBuilding(this);
			rawAgent->mPosition = SectorPosition(sector.get(), localX, localY);
			rawAgent->mResetPosition = rawAgent->mPosition;
			sector->mAgents.insert(rawAgent);
			mAgents.restore(id, std::move(agent));
			mAgentIds.emplace(rawAgent, id);

			if (destinationSectorIndex)
			{
				if (*destinationSectorIndex >= mSectors.size()
					|| !isfinite(destinationLocalX) || !isfinite(destinationLocalY))
				{
					throw SerializationException("Serialized Agent path has an invalid destination");
				}
				auto const& destinationSector = mSectors[*destinationSectorIndex];
				auto destinationPosition = destinationSector->getPosition()
					+ Vector2{ destinationLocalX, destinationLocalY };
				auto destination = mGraph->getClosestVertexInSector(
					destinationSector.get(), destinationPosition);
				auto path = mGraph->calculatePath(rawAgent, destination);
				if (!path || path->nodes.empty())
				{
					throw SerializationException("Serialized Agent path destination is unreachable");
				}
				rawAgent->assignPath(std::move(path), pathActive, false);
				rawAgent->mResetPath = rawAgent->mPath.path;
				rawAgent->mResetPathActive = pathActive;
			}
		}
		serializer.endArray();
		serializer.endMap();
		return true;
	}

	void Building::resetSimulation()
	{
		auto const wasModified = isModified();
		auto const wasPaused = mSimulationPaused;

		auto output = YamlSerializer::toString();
		SerializationWorkData writeData;
		writeData.markSerializedUnmodified = false;
		serialize(*output, writeData);
		output->serialize();

		auto input = YamlSerializer::fromString(output->getSerializedString());
		input->deserialize();
		SerializationWorkData readData;
		deserialize(*input, readData);
		if (wasModified) markModified();
		if (wasPaused) pauseSimulation();
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
		for (uint32_t layer = 0; layer < mLayers.size(); ++layer)
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

	std::unique_ptr<Building> Building::makeCandidateBuilding() const
	{
		auto candidate = std::make_unique<Building>(mName, mCellsWide, mDecksHigh);
		while (candidate->getLayerCount() < getLayerCount())
			candidate->addLayer();
		for (uint32_t layer = 2; layer < getLayerCount(); ++layer)
			candidate->setLayerName(layer, mLayerNames[layer]);
		return candidate;
	}

	void Building::applyConstructionRecord(ConstructionRecord const& record)
	{
		// A record written before Transits and Doors carried a Layer replayed against
		// the front pair, which is the only pair a two-Layer Building could express.
		auto const transitLayer = [](ConstructionRecord const& r) -> uint32_t
		{
			return r.layer == ~0u ? layerBehind(0) : r.layer;
		};
		auto const doorLayer = [](ConstructionRecord const& r) -> uint32_t
		{
			return r.layer == ~0u ? 0u : r.layer;
		};

		switch (record.type)
		{
		case ConstructionType::Corridor:
			addCorridor(record.layer == ~0u ? 0u : record.layer, record.a, record.b, record.c, record.d);
			break;
		case ConstructionType::Room:
			addRoom(record.name, record.a, record.b, record.c, record.d, record.e, record.x);
			break;
		case ConstructionType::Ladder:
			addLadder(transitLayer(record), record.a, record.b,
				{ record.c, record.p, record.q, record.d });
			break;
		case ConstructionType::Stairwell:
			addStairwell(transitLayer(record), record.a, record.b,
				{ record.c, record.i, record.d, record.e });
			break;
		case ConstructionType::Staircase:
			addStaircase(transitLayer(record), record.a, record.b, { record.c, record.i, record.x });
			break;
		case ConstructionType::Lift:
			addLift(transitLayer(record), record.a, record.b,
				{ record.c, record.values, record.d, record.x, record.y, record.g, record.e });
			break;
		case ConstructionType::Shuttle:
			addShuttle(transitLayer(record), record.a, record.b, record.c,
				{ record.d, record.e, record.values, record.f, record.g, record.x, record.y,
					record.p, record.h ? record.h : (1u << 1) });
			break;
		case ConstructionType::Door:
			addSectorDoor(doorLayer(record), record.a, record.b,
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
				{ record.d, record.p, record.q, record.e });
			break;
		case ConstructionType::PlatformLift:
			addSectorPlatformLift(record.a, record.b, record.c,
				{ record.d, record.values, record.e, record.x, record.y, 0, 0, record.z });
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
				|| type == ConstructionType::Ladder || type == ConstructionType::Stairwell || type == ConstructionType::Staircase
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
		// A wall removal is a Location prerequisite, not a Location payload: a
		// Staircase or Escalator validates against the wall being open as it is
		// created, so removals have to replay before Transits rather than after them.
		auto isLocationPrerequisite = [](ConstructionType type)
		{
			return type == ConstructionType::RemoveWall;
		};
		struct Item { ConstructionRecord record; uint32_t oldSector{ ~0u }; };
		vector<Item> locations, transits, other;
		uint32_t oldSector = 0;
		for (auto& record : records)
		{
			bool const producer = createsSector(record.type);
			Item item{ std::move(record), producer ? oldSector++ : ~0u };
			if (isLocation(item.record.type) || isLocationPrerequisite(item.record.type))
				locations.push_back(std::move(item));
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
				|| it->type == ConstructionType::Ladder || it->type == ConstructionType::Stairwell || it->type == ConstructionType::Staircase
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
			auto candidate = makeCandidateBuilding();
			candidate->mDeserializingConstruction = true;
			for (auto const& record : records) candidate->applyConstructionRecord(record);
			candidate->finishBuild();
		}
		catch (Exception const& error) { diagnostic = error.getMessage(); return false; }
		catch (exception const& error) { diagnostic = error.what(); return false; }
		return true;
	}

	void Building::rebuildFromConstructionRecords(vector<ConstructionRecord> records,
		uint32_t movedSectorIndex, int deltaX, int deltaY)
	{
		struct ActiveLadder { uint32_t sector, deck, x, height; };
		vector<ActiveLadder> activeLadders;
		for (auto const& record : mConstructionRecords)
		{
			if (record.type != ConstructionType::SectorLadder || record.a >= mSectors.size()) continue;
			auto sector = mSectors[record.a];
			for (uint32_t i = 0; sector && i < sector->getNumObjects(); ++i)
			{
				auto object = dynamic_pointer_cast<LadderSectorObject>(sector->getObject(i));
				if (object && object->getCellX() == sector->getCellX() + record.c
					&& object->getCellY() == sector->getCellY() + record.b
					&& roomLadderIsActive(object->getLadder()))
					activeLadders.push_back({ record.a, record.b, record.c, record.d });
			}
		}
		string ladderDiagnostic;
		if (!normalizeRoomLadderRecords(records, ladderDiagnostic))
			throw BuildingException(this, ladderDiagnostic);
		for (auto const& active : activeLadders)
		{
			auto unchanged = find_if(records.begin(), records.end(), [&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::SectorLadder && record.a == active.sector
					&& record.b == active.deck && record.c == active.x && record.d == active.height;
			});
			if (unchanged == records.end())
				throw BuildingException(this, "A Room Ladder cannot be changed while it is in use");
		}
		// Validate the complete replay before touching the live Building. This also
		// makes dependent Walkway/Ladder edits transactional.
		try
		{
			auto candidate = makeCandidateBuilding();
			candidate->mDeserializingConstruction = true;
			for (auto const& record : records) candidate->applyConstructionRecord(record);
			candidate->finishBuild();
		}
		catch (Exception const&) { throw; }
		catch (exception const& error) { throw BuildingException(this, error.what()); }

		struct SavedAgent { AgentId id; string name; uint32_t flags; uint32_t layer; Vector2 position; };
		vector<SavedAgent> agents;
		for (auto const& [id, agent] : mAgents.entries())
		{
			auto position = agent->getGlobalPosition();
			if (agent->getSector() && agent->getSector()->getIndex() == movedSectorIndex)
				position += Vector2{ (float)deltaX, (float)deltaY };
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

	set<uint32_t> Building::thresholdLayers(SectorObjectType type, uint32_t x, uint32_t y) const
	{
		set<uint32_t> layers;
		for (auto const& sector : mSectors)
		{
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (!object || object->getObjectType() != type) continue;
				if (object->getCellX() != x || object->getCellY() != y) continue;
				layers.insert(sector->getLayerIndex());
			}
		}
		return layers;
	}

	vector<Building::ConstructionRecord> Building::recordsWithoutLayer(uint32_t layerIndex,
		LayerDeleteImpact& impact) const
	{
		impact = {};
		impact.sectorRemoved.assign(mSectors.size(), false);

		// A Transit on layer N lands on the Layer in front of it, so deleting a Layer
		// also breaks the Transits one Layer *behind* the deletion.  layerBehind()
		// still asserts the two-Layer back boundary, so the neighbour is derived here.
		auto const behind = layerIndex + 1;

		auto isTransitRecord = [](ConstructionType type)
		{
			return type == ConstructionType::Ladder || type == ConstructionType::Stairwell
				|| type == ConstructionType::Staircase || type == ConstructionType::Lift
				|| type == ConstructionType::Shuttle;
		};
		auto isSectorReference = [](ConstructionType type)
		{
			return type == ConstructionType::LightSwitch || type == ConstructionType::ForceBridge
				|| type == ConstructionType::SectorLadder || type == ConstructionType::PlatformLift
				|| type == ConstructionType::Walkway || type == ConstructionType::Marker
				|| type == ConstructionType::RemoveWall || type == ConstructionType::RemoveMarker
				|| type == ConstructionType::ObjectTombstone;
		};

		// Sectors are created one per record, in record order, so a producing
		// record's position among the producers is its live Sector index.  Decide
		// which Sectors survive first, and which index each one takes in the
		// compacted Building, so that records pointing at a Sector can be re-pointed
		// against the *original* numbering rather than a renumbered copy of it.
		vector<bool> keepRecord(mConstructionRecords.size(), true);
		vector<uint32_t> sectorMap(mSectors.size(), ~0u);
		uint32_t producerIndex = 0;
		uint32_t nextSector = 0;

		for (size_t i = 0; i < mConstructionRecords.size(); ++i)
		{
			auto const& record = mConstructionRecords[i];
			bool keep = true;

			switch (record.type)
			{
			case ConstructionType::Corridor:
			case ConstructionType::Room:
			case ConstructionType::Ladder:
			case ConstructionType::Stairwell:
			case ConstructionType::Staircase:
			case ConstructionType::Lift:
			case ConstructionType::Shuttle:
			{
				bool const transit = isTransitRecord(record.type);
				auto const layer = producerIndex < mSectors.size() && mSectors[producerIndex]
						? mSectors[producerIndex]->getLayerIndex() : layerIndex;
				keep = layer != layerIndex && !(transit && layer == behind);
				if (producerIndex < impact.sectorRemoved.size())
					impact.sectorRemoved[producerIndex] = !keep;
				if (keep) sectorMap[producerIndex] = nextSector++;
				else if (transit) ++impact.transitsRemoved;
				else ++impact.locationsRemoved;
				++producerIndex;
				break;
			}
			case ConstructionType::Door:
				// A Door record carries no Layer field; the Layers it really crosses
				// come from the Sectors which hold the Door object.
				keep = thresholdLayers(SectorObjectType::Door, record.b, record.a)
					.count(layerIndex) == 0;
				if (!keep) ++impact.doorsRemoved;
				break;
			case ConstructionType::Window:
				keep = record.a != layerIndex
					&& thresholdLayers(SectorObjectType::Window, record.c, record.b)
						.count(layerIndex) == 0;
				if (!keep) ++impact.windowsRemoved;
				break;
			case ConstructionType::BulkheadDoor:
				// A Bulkhead Door joins two Locations on its own Layer.
				keep = record.a != layerIndex;
				break;
			default:
				// Everything else is authored against a Sector and dies with it.
				keep = record.a < sectorMap.size() && sectorMap[record.a] != ~0u;
				break;
			}

			keepRecord[i] = keep;
		}

		// The authored record order is preserved.  It is the order the Building was
		// built and replayed in, and that order carries dependencies: a wall removal
		// has to precede the Staircase which needs the wall to be open.
		vector<ConstructionRecord> records;
		records.reserve(mConstructionRecords.size());

		for (size_t i = 0; i < mConstructionRecords.size(); ++i)
		{
			if (!keepRecord[i]) continue;

			auto record = mConstructionRecords[i];
			switch (record.type)
			{
			case ConstructionType::Room:
			case ConstructionType::Window:
			case ConstructionType::BulkheadDoor:
				if (record.a > layerIndex) record.a -= 1;
				break;
			case ConstructionType::Corridor:
			case ConstructionType::Ladder:
			case ConstructionType::Stairwell:
			case ConstructionType::Staircase:
			case ConstructionType::Lift:
			case ConstructionType::Shuttle:
			case ConstructionType::Door:
				// These records carry the Layer they are authored on, so a deletion in
				// front of them has to pull that Layer forward with every other one.
				if (record.layer != ~0u && record.layer > layerIndex) record.layer -= 1;
				break;
			default:
				if (isSectorReference(record.type) && record.a < sectorMap.size())
					record.a = sectorMap[record.a];
				break;
			}
			records.push_back(std::move(record));
		}

		return records;
	}

	Building::LayerDeletePlan Building::planDeleteLayer(uint32_t layerIndex) const
	{
		LayerDeletePlan plan;
		plan.layerIndex = layerIndex;
		plan.layerCountBefore = getLayerCount();
		plan.layerCountAfter = plan.layerCountBefore - 1;

		if (layerIndex >= plan.layerCountBefore)
		{
			plan.diagnostic = format("There is no Layer {} to delete", layerIndex);
			return plan;
		}
		if (plan.layerCountAfter < 2)
		{
			plan.diagnostic = "A Building must keep at least two Layers";
			return plan;
		}

		plan.layerName = mLayerNames[layerIndex];

		LayerDeleteImpact impact;
		vector<ConstructionRecord> records;
		try
		{
			records = recordsWithoutLayer(layerIndex, impact);
		}
		catch (Exception const& error) { plan.diagnostic = error.getMessage(); return plan; }
		catch (exception const& error) { plan.diagnostic = error.what(); return plan; }

		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			auto const* sector = agent->getSector();
			if (!sector) continue;
			auto const index = sector->getIndex();
			if (index < impact.sectorRemoved.size() && impact.sectorRemoved[index])
				++plan.agentsRemoved;
		}

		// The rewritten records must rebuild a valid Building before their
		// consequences can be offered to the user as a confirmed edit.
		try
		{
			auto candidate = makeCandidateBuilding();
			candidate->mDeserializingConstruction = true;
			for (auto const& record : records) candidate->applyConstructionRecord(record);
			candidate->finishBuild();
		}
		catch (Exception const& error) { plan.diagnostic = error.getMessage(); return plan; }
		catch (exception const& error) { plan.diagnostic = error.what(); return plan; }

		plan.locationsRemoved = impact.locationsRemoved;
		plan.transitsRemoved = impact.transitsRemoved;
		plan.doorsRemoved = impact.doorsRemoved;
		plan.windowsRemoved = impact.windowsRemoved;

		if (plan.locationsRemoved > 0)
			plan.consequences.push_back(format("Delete {} Sector{} on {}",
				plan.locationsRemoved, plan.locationsRemoved == 1 ? "" : "s", plan.layerName));
		if (plan.transitsRemoved > 0)
		{
			auto const targets = layerIndex + 1 < plan.layerCountBefore
				? format("{} and {}", plan.layerName, mLayerNames[layerIndex + 1])
				: plan.layerName;
			plan.consequences.push_back(format("Delete {} Transit{} on {}",
				plan.transitsRemoved, plan.transitsRemoved == 1 ? "" : "s", targets));
		}
		if (plan.doorsRemoved > 0)
			plan.consequences.push_back(format("Delete {} Door{} crossing {}",
				plan.doorsRemoved, plan.doorsRemoved == 1 ? "" : "s", plan.layerName));
		if (plan.windowsRemoved > 0)
			plan.consequences.push_back(format("Delete {} Window{} crossing {}",
				plan.windowsRemoved, plan.windowsRemoved == 1 ? "" : "s", plan.layerName));
		if (plan.agentsRemoved > 0)
			plan.consequences.push_back(format("Remove {} Agent{} in the deleted Sectors",
				plan.agentsRemoved, plan.agentsRemoved == 1 ? "" : "s"));
		for (uint32_t layer = layerIndex + 1; layer < plan.layerCountBefore; ++layer)
			plan.consequences.push_back(format("{} moves from Layer {} to Layer {}",
				mLayerNames[layer], layer, layer - 1));
		if (plan.consequences.empty())
			plan.consequences.push_back(format("{} is removed; nothing was on it",
				plan.layerName));

		plan.valid = true;
		return plan;
	}

	bool Building::applyDeleteLayer(LayerDeletePlan const& requested)
	{
		auto const plan = planDeleteLayer(requested.layerIndex);
		if (!plan.valid) throw BuildingException(this, plan.diagnostic);

		LayerDeleteImpact impact;
		auto records = recordsWithoutLayer(plan.layerIndex, impact);

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
			auto const* sector = agent->getSector();
			if (!sector) continue;
			auto const index = sector->getIndex();
			if (index < impact.sectorRemoved.size() && impact.sectorRemoved[index]) continue;
			auto layer = sector->getLayerIndex();
			if (layer > plan.layerIndex) layer -= 1;
			agents.push_back({ id, agent->getName(), agent->getFlags(), layer,
				agent->getGlobalPosition() });
		}

		// Compact the Layer storage before the reset so the surviving Layers are
		// recreated at their new depth.
		mLayers.erase(mLayers.begin() + plan.layerIndex);
		mLayerNames.erase(mLayerNames.begin() + plan.layerIndex);

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

		return true;
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
				auto occupant = mLayers[layerBehind(0)]->getCellDefinition(ix, iy).sectorIndex;
				if (occupant != ~0u && occupant != sectorIndex)
				{ plan.diagnostic = format("Sector at {},{} blocks the Lift", ix, iy); return plan; }
			}
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			auto const& first = mLayers[0]->getCellDefinition(x, iy);
			if (first.sectorIndex == ~0u) continue;
			auto location = dynamic_pointer_cast<const Location>(mSectors[first.sectorIndex]);
			if (!location) continue;
			bool complete = true;
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cell = mLayers[0]->getCellDefinition(ix, iy);
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
		{ plan.diagnostic = "The Lift requires at least two fully overlapping Fore-layer Location floors"; return plan; }
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
		auto const& cell = mLayers[layerBehind(0)]->getCellDefinition(plan.x, plan.y);
		return cell.sectorIndex;
	}

	bool Building::prepareShuttleEdit(ShuttleEditPlan const& plan,
		vector<ConstructionRecord>& records, string& diagnostic) const
	{
		records = mConstructionRecords;
		uint32_t producerIndex = 0;
		auto found = records.end();
		for (auto it = records.begin(); it != records.end(); ++it)
		{
			bool producer = it->type == ConstructionType::Corridor || it->type == ConstructionType::Room
				|| it->type == ConstructionType::Ladder || it->type == ConstructionType::Stairwell || it->type == ConstructionType::Staircase
				|| it->type == ConstructionType::Lift || it->type == ConstructionType::Shuttle;
			if (!producer) continue;
			if (producerIndex++ == plan.sectorIndex) { found = it; break; }
		}
		if (found == records.end() || found->type != ConstructionType::Shuttle)
		{
			diagnostic = "The selected Shuttle no longer has an authored definition";
			return false;
		}
		if (plan.remove)
		{
			records.erase(found);
			// Windows require occupied geometry on both layers. Any Window touching
			// this Shuttle would become unreplayable once its Back-layer sector is
			// removed, so delete that dependent authored object in the same edit.
			records.erase(remove_if(records.begin(), records.end(), [&](ConstructionRecord const& record)
			{
				if (record.type != ConstructionType::Window) return false;
				for (uint32_t iy = record.b; iy < record.b + record.e && iy < mDecksHigh; ++iy)
					for (uint32_t ix = record.c; ix < record.c + record.d && ix < mCellsWide; ++ix)
						if (mLayers[layerBehind(0)]->getCellDefinition(ix, iy).sectorIndex
							== plan.sectorIndex) return true;
				return false;
			}), records.end());
		}
		else
		{
			auto oldCurrentStop = found->f;
			auto oldPosition = (float)(found->b + found->values[oldCurrentStop]);
			for (auto const& [id, resource] : mTraversalResources.entries())
			{
				(void)id;
				if (resource->mShuttle && resource->mLiftSector.value == (uint64_t)plan.sectorIndex + 1)
				{ oldPosition = resource->mLiftPosition; oldCurrentStop = resource->mLiftCurrentStop; break; }
			}
			found->a = plan.y; found->b = plan.x; found->c = plan.cellsWide;
			found->values = plan.stopOffsets;
			auto nearest = min_element(found->values.begin(), found->values.end(), [&](auto a, auto b)
			{
				auto da = abs((float)(plan.x + a) - oldPosition);
				auto db = abs((float)(plan.x + b) - oldPosition);
				return da == db ? a < b : da < db;
			});
			found->f = plan.move && oldCurrentStop < found->values.size()
				? oldCurrentStop : (uint32_t)distance(found->values.begin(), nearest);
		}
		records = canonicalConstructionRecords(std::move(records));
		try
		{
			auto candidate = makeCandidateBuilding();
			candidate->mDeserializingConstruction = true;
			for (auto const& record : records) candidate->applyConstructionRecord(record);
			candidate->finishBuild();
		}
		catch (Exception const& error) { diagnostic = error.getMessage(); return false; }
		catch (exception const& error) { diagnostic = error.what(); return false; }
		return true;
	}

	Building::ShuttleEditPlan Building::planResizeShuttle(uint32_t sectorIndex,
		uint32_t x, uint32_t y, uint32_t cellsWide) const
	{
		ShuttleEditPlan plan;
		plan.sectorIndex = sectorIndex; plan.x = x; plan.y = y; plan.cellsWide = cellsWide;
		if (sectorIndex >= mSectors.size() || !dynamic_pointer_cast<const ShuttleTransit>(mSectors[sectorIndex]))
		{ plan.diagnostic = "Only Shuttles can be resized"; return plan; }
		auto shuttleTransit = dynamic_pointer_cast<const ShuttleTransit>(mSectors[sectorIndex]);
		auto shuttle = shuttleTransit->getShuttle();
		plan.move = cellsWide == shuttleTransit->getCellsWide()
			&& (x != shuttleTransit->getCellX() || y != shuttleTransit->getCellY());
		if (cellsWide == 0 || x + cellsWide > mCellsWide || y >= mDecksHigh)
		{ plan.diagnostic = "The Shuttle track is outside the Building bounds"; return plan; }
		if (!shuttleTransit->getAgents().empty())
		{ plan.diagnostic = "The Shuttle cannot be edited while agents occupy it"; return plan; }

		ConstructionRecord const* authored = nullptr;
		uint32_t producerIndex = 0;
		for (auto const& record : mConstructionRecords)
		{
			bool producer = record.type == ConstructionType::Corridor || record.type == ConstructionType::Room
				|| record.type == ConstructionType::Ladder || record.type == ConstructionType::Stairwell || record.type == ConstructionType::Staircase
				|| record.type == ConstructionType::Lift || record.type == ConstructionType::Shuttle;
			if (!producer) continue;
			if (producerIndex++ == sectorIndex) { authored = &record; break; }
		}
		if (!authored || authored->type != ConstructionType::Shuttle)
		{ plan.diagnostic = "The selected Shuttle no longer has an authored definition"; return plan; }
		auto shuttleWidth = authored->d * authored->e + authored->d - 1;
		if (cellsWide < shuttleWidth)
		{ plan.diagnostic = "The Shuttle track is shorter than the coupled vehicle"; return plan; }

		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			if (!resource->mShuttle || resource->mLiftSector.value != (uint64_t)sectorIndex + 1) continue;
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
			{ plan.diagnostic = "The Shuttle cannot be edited while it has active journeys, queues, or reservations"; return plan; }
		}
		for (uint32_t ix = x; ix < x + cellsWide; ++ix)
		{
			auto occupant = mLayers[layerBehind(0)]->getCellDefinition(ix, y).sectorIndex;
			if (occupant != ~0u && occupant != sectorIndex)
			{ plan.diagnostic = format("Sector at {},{} blocks the Shuttle", ix, y); return plan; }
		}

		auto supported = [&](uint32_t offset)
		{
			if (offset + shuttleWidth > cellsWide) return false;
			bool any = false, all = true;
			auto doorMask = authored->h ? authored->h : (1u << 1);
			for (uint32_t car = 0; car < authored->d; ++car)
				for (uint32_t doorOffset = 0; doorOffset < authored->e; ++doorOffset)
				{
					if ((doorMask & (1u << doorOffset)) == 0) continue;
					auto doorX = x + offset + car * (authored->e + 1) + doorOffset;
					auto const& cell = mLayers[0]->getCellDefinition(doorX, y);
					bool valid = cell.sectorIndex != ~0u
						&& mSectors[cell.sectorIndex]->getType() == SectorType::Location
						&& cell.isTraversableOnFoot() && cell.markers.empty();
					if (valid && cell.hasObject())
					{
						auto object = mSectors[cell.sectorIndex]->getObject(cell.sectorObjectIndex);
						uint32_t owner;
						valid = (isShuttleOwnedDoor(object, &owner) || isShuttleOwnedControl(object, &owner))
							&& owner == sectorIndex;
					}
					if (valid)
					{
						auto sector = mSectors[cell.sectorIndex];
						valid = doorX != sector->getCellX0() || doorX != sector->getCellX1();
					}
					any = any || valid; all = all && valid;
				}
			return any && (authored->p || all);
		};

		vector<uint32_t> oldGlobalStops, newGlobalStops;
		for (auto offset : authored->values) oldGlobalStops.push_back(authored->b + offset);
		for (auto offset : authored->values)
		{
			int64_t transformed = plan.move ? offset
				: (int64_t)authored->b + offset - (int64_t)x;
			if (transformed < 0 || transformed > UINT32_MAX || !supported((uint32_t)transformed)) continue;
			plan.stopOffsets.push_back((uint32_t)transformed);
			newGlobalStops.push_back(x + (uint32_t)transformed);
		}
		if (plan.stopOffsets.size() < 2)
		{ plan.diagnostic = "The Shuttle requires at least two valid platform stops"; return plan; }
		for (size_t i = 1; i < plan.stopOffsets.size(); ++i)
			if (plan.stopOffsets[i] - plan.stopOffsets[i - 1] < shuttleWidth)
			{ plan.diagnostic = "Shuttle stops must be separated by at least the coupled vehicle width"; return plan; }

		if (plan.move) plan.consequences.push_back("Move the Shuttle and rebuild every landing door and button");
		else if (x != shuttleTransit->getCellX() || cellsWide != shuttleTransit->getCellsWide())
			plan.consequences.push_back("Resize the Shuttle track and rebuild affected landings");
		if (!plan.move)
		{
			for (auto global : oldGlobalStops)
				if (find(newGlobalStops.begin(), newGlobalStops.end(), global) == newGlobalStops.end())
					plan.consequences.push_back(format("Remove Shuttle stop and landings at position {}", global));
			for (auto const& [id, resource] : mTraversalResources.entries())
			{
				(void)id;
				if (!resource->mShuttle || resource->mLiftSector.value != (uint64_t)sectorIndex + 1) continue;
				if (find(newGlobalStops.begin(), newGlobalStops.end(), (uint32_t)round(resource->mLiftPosition))
					== newGlobalStops.end())
					plan.consequences.push_back("Relocate the Shuttle to the nearest remaining stop");
			}
		}
		vector<ConstructionRecord> records;
		plan.valid = prepareShuttleEdit(plan, records, plan.diagnostic);
		return plan;
	}

	Building::ShuttleEditPlan Building::planRemoveShuttle(uint32_t sectorIndex) const
	{
		if (sectorIndex >= mSectors.size() || !dynamic_pointer_cast<const ShuttleTransit>(mSectors[sectorIndex]))
		{ ShuttleEditPlan plan; plan.diagnostic = "Only a Shuttle can be deleted"; return plan; }
		auto transit = dynamic_pointer_cast<const ShuttleTransit>(mSectors[sectorIndex]);
		auto plan = planResizeShuttle(sectorIndex, transit->getCellX(), transit->getCellY(), transit->getCellsWide());
		if (!plan.valid) return plan;
		plan.remove = true; plan.consequences.clear();
		for (uint32_t stop = 0; stop < transit->getNumStops(); ++stop)
			plan.consequences.push_back(format("Delete Shuttle stop {} and all carriage landings", stop));
		for (auto const& record : mConstructionRecords)
		{
			if (record.type != ConstructionType::Window) continue;
			bool dependent = false;
			for (uint32_t iy = record.b; !dependent && iy < record.b + record.e && iy < mDecksHigh; ++iy)
				for (uint32_t ix = record.c; ix < record.c + record.d && ix < mCellsWide; ++ix)
					if (mLayers[layerBehind(0)]->getCellDefinition(ix, iy).sectorIndex == sectorIndex)
					{ dependent = true; break; }
			if (dependent) plan.consequences.push_back(format(
				"Delete dependent Window at {},{} ({} x {} cells)", record.c, record.b, record.d, record.e));
		}
		vector<ConstructionRecord> records;
		plan.valid = prepareShuttleEdit(plan, records, plan.diagnostic);
		return plan;
	}

	Building::ShuttleEditPlan Building::planRemoveShuttleStop(uint32_t sectorIndex, uint32_t stopIndex) const
	{
		ShuttleEditPlan invalid;
		if (sectorIndex >= mSectors.size()) { invalid.diagnostic = "The selected Shuttle no longer exists"; return invalid; }
		auto transit = dynamic_pointer_cast<const ShuttleTransit>(mSectors[sectorIndex]);
		if (!transit || stopIndex >= transit->getNumStops())
		{ invalid.diagnostic = "The selected Shuttle stop no longer exists"; return invalid; }
		if (transit->getNumStops() <= 2)
		{ invalid.diagnostic = "Deleting this landing would leave the Shuttle with fewer than two stops"; return invalid; }
		auto plan = planResizeShuttle(sectorIndex, transit->getCellX(), transit->getCellY(), transit->getCellsWide());
		if (!plan.valid) return plan;
		plan.stopOffsets.clear();
		for (uint32_t stop = 0; stop < transit->getNumStops(); ++stop)
		{
			if (stop == stopIndex) continue;
			auto const& value = transit->getStop(stop);
			plan.stopOffsets.push_back((uint32_t)((int)value.sector->getCellX()
				+ value.sectorOffsetX - (int)transit->getCellX()));
		}
		plan.consequences = { format("Delete Shuttle stop {} and all carriage doors, buttons, access zones, and pathing", stopIndex) };
		vector<ConstructionRecord> records;
		plan.valid = prepareShuttleEdit(plan, records, plan.diagnostic);
		return plan;
	}

	Building::ShuttleEditPlan Building::planAddShuttleStop(uint32_t sectorIndex, uint32_t stopOffset) const
	{
		ShuttleEditPlan invalid;
		if (sectorIndex >= mSectors.size()) { invalid.diagnostic = "The selected Shuttle no longer exists"; return invalid; }
		auto transit = dynamic_pointer_cast<const ShuttleTransit>(mSectors[sectorIndex]);
		if (!transit) { invalid.diagnostic = "The selected sector is not a Shuttle"; return invalid; }
		auto plan = planResizeShuttle(sectorIndex, transit->getCellX(), transit->getCellY(), transit->getCellsWide());
		if (!plan.valid) return plan;
		if (find(plan.stopOffsets.begin(), plan.stopOffsets.end(), stopOffset) != plan.stopOffsets.end())
		{ invalid.diagnostic = "The Shuttle already has this stop"; return invalid; }
		plan.stopOffsets.push_back(stopOffset);
		sort(plan.stopOffsets.begin(), plan.stopOffsets.end());
		plan.consequences = { format("Create Shuttle stop and supported carriage landings at position {}",
			transit->getCellX() + stopOffset) };
		vector<ConstructionRecord> records;
		plan.valid = prepareShuttleEdit(plan, records, plan.diagnostic);
		return plan;
	}

	uint32_t Building::applyShuttleEdit(ShuttleEditPlan const& requested)
	{
		if (!mSimulationPaused) throw BuildingException(this, "Editing a Shuttle requires the simulation to be paused");
		auto plan = requested.remove ? planRemoveShuttle(requested.sectorIndex)
			: planResizeShuttle(requested.sectorIndex, requested.x, requested.y, requested.cellsWide);
		if (!plan.valid) throw BuildingException(this, plan.diagnostic);
		if (!requested.remove && requested.stopOffsets.size() >= 2
			&& requested.stopOffsets != plan.stopOffsets)
		{
			plan.stopOffsets = requested.stopOffsets;
			plan.consequences = requested.consequences;
			vector<ConstructionRecord> validationRecords;
			if (!prepareShuttleEdit(plan, validationRecords, plan.diagnostic))
				throw BuildingException(this, plan.diagnostic);
		}
		vector<ConstructionRecord> records; string diagnostic;
		if (!prepareShuttleEdit(plan, records, diagnostic)) throw BuildingException(this, diagnostic);
		rebuildFromConstructionRecords(std::move(records));
		if (plan.remove) return ~0u;
		return mLayers[layerBehind(0)]->getCellDefinition(plan.x, plan.y).sectorIndex;
	}

	bool Building::getLadderOptions(uint32_t sectorIndex, CreateLadderOptions& options) const
	{
		uint32_t producerIndex = 0;
		for (auto const& record : mConstructionRecords)
		{
			bool producer = record.type == ConstructionType::Corridor || record.type == ConstructionType::Room
				|| record.type == ConstructionType::Ladder || record.type == ConstructionType::Stairwell || record.type == ConstructionType::Staircase
				|| record.type == ConstructionType::Lift || record.type == ConstructionType::Shuttle;
			if (!producer) continue;
			if (producerIndex++ != sectorIndex) continue;
			if (record.type != ConstructionType::Ladder) return false;
			options = { record.c, record.p, record.q, record.d };
			return true;
		}
		return false;
	}

	bool Building::prepareLadderEdit(LadderEditPlan const& plan,
		vector<ConstructionRecord>& records, string& diagnostic) const
	{
		auto createsSector = [](ConstructionType type)
		{
			return type == ConstructionType::Corridor || type == ConstructionType::Room
				|| type == ConstructionType::Ladder || type == ConstructionType::Stairwell || type == ConstructionType::Staircase
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

		records = mConstructionRecords;
		uint32_t producerIndex = 0;
		auto found = records.end();
		for (auto it = records.begin(); it != records.end(); ++it)
		{
			if (!createsSector(it->type)) continue;
			if (producerIndex++ == plan.sectorIndex) { found = it; break; }
		}
		if (found == records.end() || found->type != ConstructionType::Ladder)
		{
			diagnostic = "The selected Ladder no longer has an authored definition";
			return false;
		}
		if (plan.remove)
		{
			records.erase(found);
			records.erase(remove_if(records.begin(), records.end(), [&](auto& record)
			{
				if (!referencesSector(record.type)) return false;
				if (record.a == plan.sectorIndex) return true;
				if (record.a > plan.sectorIndex) --record.a;
				return false;
			}), records.end());
		}
		else
		{
			found->a = plan.y; found->b = plan.x; found->c = plan.options.decksHigh;
			found->p = plan.options.extensible; found->q = plan.options.startExtended;
			found->d = plan.options.directionalBatchLimit;
		}

		try
		{
			auto candidate = makeCandidateBuilding();
			candidate->mDeserializingConstruction = true;
			vector<ConstructionRecord> viable;
			viable.reserve(records.size());
			for (auto const& record : records)
			{
				try
				{
					candidate->applyConstructionRecord(record);
					viable.push_back(record);
				}
				catch (Exception const& error)
				{
					if (createsSector(record.type))
					{
						diagnostic = error.getMessage();
						return false;
					}
				}
			}
			candidate->finishBuild();
			records = std::move(viable);
		}
		catch (Exception const& error) { diagnostic = error.getMessage(); return false; }
		catch (exception const& error) { diagnostic = error.what(); return false; }
		return true;
	}

	Building::LadderEditPlan Building::planResizeLadder(uint32_t sectorIndex,
		uint32_t x, uint32_t y, CreateLadderOptions const& options) const
	{
		LadderEditPlan plan;
		plan.sectorIndex = sectorIndex; plan.x = x; plan.y = y;
		plan.decksHigh = options.decksHigh; plan.options = options;
		if (sectorIndex >= mSectors.size() || !dynamic_pointer_cast<const LadderTransit>(mSectors[sectorIndex]))
		{ plan.diagnostic = "Only Ladders can be edited"; return plan; }
		auto ladder = dynamic_pointer_cast<const LadderTransit>(mSectors[sectorIndex]);
		plan.move = options.decksHigh == ladder->getDecksHigh()
			&& (x != ladder->getCellX() || y != ladder->getCellY());
		if (options.decksHigh < 2)
		{ plan.diagnostic = "A Ladder must span at least two decks"; return plan; }
		if (options.directionalBatchLimit == 0)
		{ plan.diagnostic = "Ladder directional batch limit must be positive"; return plan; }
		if (x >= mCellsWide || y >= mDecksHigh || y + options.decksHigh > mDecksHigh)
		{ plan.diagnostic = "The Ladder is outside the Building bounds"; return plan; }
		for (uint32_t iy = y; iy < y + options.decksHigh; ++iy)
		{
			auto occupant = mLayers[layerBehind(0)]->getCellDefinition(x, iy).sectorIndex;
			if (occupant != ~0u && occupant != sectorIndex)
			{ plan.diagnostic = format("A Back-layer Sector at {},{} blocks the Ladder", x, iy); return plan; }
		}
		auto upperY = y + options.decksHigh - 1;
		auto const& lower = mLayers[0]->getCellDefinition(x, y);
		auto const& upper = mLayers[0]->getCellDefinition(x, upperY);
		if (lower.sectorIndex == ~0u || !mSectors[lower.sectorIndex]
			|| mSectors[lower.sectorIndex]->getType() != SectorType::Location)
		{ plan.diagnostic = format("A Fore-layer Location is required at {},{}", x, y); return plan; }
		if (upper.sectorIndex == ~0u || !mSectors[upper.sectorIndex]
			|| mSectors[upper.sectorIndex]->getType() != SectorType::Location)
		{ plan.diagnostic = format("A Fore-layer Location is required at {},{}", x, upperY); return plan; }
		if (lower.sectorIndex == upper.sectorIndex)
		{ plan.diagnostic = "A Ladder must connect two different Fore-layer Locations"; return plan; }
		if (!lower.isTraversableOnFoot())
		{ plan.diagnostic = format("The Fore-layer floor at {},{} is not traversable", x, y); return plan; }
		if (!upper.isTraversableOnFoot())
		{ plan.diagnostic = format("The Fore-layer floor at {},{} is not traversable", x, upperY); return plan; }
		auto crossedFloors = (float)(options.decksHigh - 1);
		auto agentSpacing = CORE_LADDER_AGENT_SPACING / CORE_CELL_YX_RENDER_RATIO;
		auto capacity = max(1u, (uint32_t)floor(crossedFloors / agentSpacing));
		if (ladder->getAgents().size() > capacity)
		{ plan.diagnostic = "Ladder capacity is below its current occupancy"; return plan; }

		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent->getSector() != ladder.get() || plan.move) continue;
			auto position = agent->getGlobalPosition();
			if (position.y < y || position.y >= y + options.decksHigh)
				plan.consequences.push_back("Delete Agent " + agent->getName());
		}
		vector<ConstructionRecord> records;
		plan.valid = prepareLadderEdit(plan, records, plan.diagnostic);
		if (plan.valid && records.size() < mConstructionRecords.size())
			plan.consequences.push_back(format("Delete {} dependent authored object(s)",
				mConstructionRecords.size() - records.size()));
		return plan;
	}

	Building::LadderEditPlan Building::planRemoveLadder(uint32_t sectorIndex) const
	{
		LadderEditPlan plan;
		plan.remove = true; plan.sectorIndex = sectorIndex;
		if (sectorIndex >= mSectors.size() || !dynamic_pointer_cast<const LadderTransit>(mSectors[sectorIndex]))
		{ plan.diagnostic = "Only a Ladder can be deleted"; return plan; }
		auto ladder = dynamic_pointer_cast<const LadderTransit>(mSectors[sectorIndex]);
		plan.x = ladder->getCellX(); plan.y = ladder->getCellY();
		if (!getLadderOptions(sectorIndex, plan.options))
		{ plan.diagnostic = "The selected Ladder no longer has an authored definition"; return plan; }
		plan.decksHigh = plan.options.decksHigh;
		plan.consequences.push_back("Delete Ladder");
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent->getSector() == ladder.get())
				plan.consequences.push_back("Delete Agent " + agent->getName());
		}
		vector<ConstructionRecord> records;
		plan.valid = prepareLadderEdit(plan, records, plan.diagnostic);
		if (plan.valid && records.size() + 1 < mConstructionRecords.size())
			plan.consequences.push_back(format("Delete {} dependent authored object(s)",
				mConstructionRecords.size() - records.size() - 1));
		return plan;
	}

	uint32_t Building::applyLadderEdit(LadderEditPlan const& requested)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Editing a Ladder requires the simulation to be paused");
		auto plan = requested.remove ? planRemoveLadder(requested.sectorIndex)
			: planResizeLadder(requested.sectorIndex, requested.x, requested.y, requested.options);
		if (!plan.valid) throw BuildingException(this, plan.diagnostic);
		vector<ConstructionRecord> records;
		string diagnostic;
		if (!prepareLadderEdit(plan, records, diagnostic)) throw BuildingException(this, diagnostic);
		auto old = mSectors[plan.sectorIndex];
		int deltaX = plan.move ? (int)plan.x - (int)old->getCellX() : 0;
		int deltaY = plan.move ? (int)plan.y - (int)old->getCellY() : 0;
		rebuildFromConstructionRecords(std::move(records),
			plan.remove ? ~0u : plan.sectorIndex, deltaX, deltaY);
		if (plan.remove) return ~0u;
		return mLayers[layerBehind(0)]->getCellDefinition(plan.x, plan.y).sectorIndex;
	}

	bool Building::getStairwellOptions(uint32_t sectorIndex, CreateStairwellOptions& options) const
	{
		uint32_t producerIndex = 0;
		for (auto const& record : mConstructionRecords)
		{
			bool producer = record.type == ConstructionType::Corridor || record.type == ConstructionType::Room
				|| record.type == ConstructionType::Ladder || record.type == ConstructionType::Stairwell || record.type == ConstructionType::Staircase
				|| record.type == ConstructionType::Lift || record.type == ConstructionType::Shuttle;
			if (!producer) continue;
			if (producerIndex++ != sectorIndex) continue;
			if (record.type != ConstructionType::Stairwell) return false;
			options = { record.c, record.i, record.d, record.e };
			return true;
		}
		return false;
	}

	bool Building::getStaircaseOptions(uint32_t sectorIndex, CreateStaircaseOptions& options) const
	{
		uint32_t producerIndex = 0;
		for (auto const& record : mConstructionRecords)
		{
			bool producer = record.type == ConstructionType::Corridor || record.type == ConstructionType::Room
				|| record.type == ConstructionType::Ladder || record.type == ConstructionType::Stairwell
				|| record.type == ConstructionType::Staircase || record.type == ConstructionType::Lift
				|| record.type == ConstructionType::Shuttle;
			if (!producer) continue;
			if (producerIndex++ != sectorIndex) continue;
			if (record.type != ConstructionType::Staircase) return false;
			options = { record.c, record.i, record.x };
			return true;
		}
		return false;
	}

	Building::StaircaseEditPlan Building::planResizeStaircase(uint32_t sectorIndex,
		uint32_t x, uint32_t y, CreateStaircaseOptions const& options) const
	{
		StaircaseEditPlan plan;
		plan.sectorIndex = sectorIndex; plan.x = x; plan.y = y; plan.options = options;
		if (sectorIndex >= mSectors.size() || !dynamic_pointer_cast<const StaircaseTransit>(mSectors[sectorIndex]))
			{ plan.diagnostic = "Only Staircases can be edited"; return plan; }
		if (options.riseSide != CORE_SIDE_LEFT && options.riseSide != CORE_SIDE_RIGHT)
			{ plan.diagnostic = "The Staircase rise direction is invalid"; return plan; }
		if (!isfinite(options.speed))
			{ plan.diagnostic = "A Staircase speed must be finite"; return plan; }
		if (options.cellsWide < 2)
			{ plan.diagnostic = "A Staircase must be at least two cells wide"; return plan; }
		if (x >= mCellsWide || y >= mDecksHigh || options.cellsWide > mCellsWide - x || y + 1 >= mDecksHigh)
			{ plan.diagnostic = "The Staircase is outside the Building bounds"; return plan; }
		// The edited Staircase keeps the Layer it already sits on.
		auto const transitLayer = mSectors[sectorIndex]->getLayerIndex();
		auto const landingLayer = layerInFront(transitLayer);
		for (uint32_t iy = y; iy <= y + 1; ++iy)
			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
			{
				auto occupant = mLayers[transitLayer]->getCellDefinition(ix, iy).sectorIndex;
				if (occupant != ~0u && occupant != sectorIndex)
					{ plan.diagnostic = format("A Sector at {},{} blocks the Staircase", ix, iy); return plan; }
			}
		uint32_t lowerX = options.riseSide == CORE_SIDE_RIGHT ? x : x + options.cellsWide - 1;
		uint32_t upperX = options.riseSide == CORE_SIDE_RIGHT ? x + options.cellsWide - 1 : x;
		if (!validateStaircaseEndpoint(landingLayer, lowerX, y, false, options.riseSide, plan.diagnostic)
			|| !validateStaircaseEndpoint(landingLayer, upperX, y + 1, true, options.riseSide, plan.diagnostic))
			return plan;
		auto old = dynamic_pointer_cast<const StaircaseTransit>(mSectors[sectorIndex]);
		plan.move = old->getCellX() != x || old->getCellY() != y;
		plan.valid = true;
		return plan;
	}

	Building::StaircaseEditPlan Building::planRemoveStaircase(uint32_t sectorIndex) const
	{
		StaircaseEditPlan plan;
		plan.sectorIndex = sectorIndex; plan.remove = true;
		if (sectorIndex >= mSectors.size() || !dynamic_pointer_cast<const StaircaseTransit>(mSectors[sectorIndex]))
			{ plan.diagnostic = "Only a Staircase can be deleted"; return plan; }
		plan.valid = getStaircaseOptions(sectorIndex, plan.options);
		if (!plan.valid) { plan.diagnostic = "The selected Staircase no longer has an authored definition"; return plan; }
		plan.consequences.push_back("Delete Staircase");
		return plan;
	}

	uint32_t Building::applyStaircaseEdit(StaircaseEditPlan const& requested)
	{
		if (!mSimulationPaused) throw BuildingException(this, "Editing a Staircase requires the simulation to be paused");
		auto plan = requested.remove ? planRemoveStaircase(requested.sectorIndex)
			: planResizeStaircase(requested.sectorIndex, requested.x, requested.y, requested.options);
		if (!plan.valid) throw BuildingException(this, plan.diagnostic);
		auto createsSector = [](ConstructionType type)
		{
			return type == ConstructionType::Corridor || type == ConstructionType::Room
				|| type == ConstructionType::Ladder || type == ConstructionType::Stairwell
				|| type == ConstructionType::Staircase || type == ConstructionType::Lift
				|| type == ConstructionType::Shuttle;
		};
		auto referencesSector = [](ConstructionType type)
		{
			return type == ConstructionType::LightSwitch || type == ConstructionType::ForceBridge
				|| type == ConstructionType::SectorLadder || type == ConstructionType::PlatformLift
				|| type == ConstructionType::Walkway || type == ConstructionType::Marker
				|| type == ConstructionType::RemoveWall || type == ConstructionType::RemoveMarker
				|| type == ConstructionType::ObjectTombstone;
		};
		auto records = mConstructionRecords;
		uint32_t producerIndex = 0;
		auto found = records.end();
		for (auto it = records.begin(); it != records.end(); ++it)
			if (createsSector(it->type) && producerIndex++ == plan.sectorIndex) { found = it; break; }
		if (found == records.end() || found->type != ConstructionType::Staircase)
			throw BuildingException(this, "The selected Staircase no longer has an authored definition");
		if (plan.remove)
		{
			records.erase(found);
			records.erase(remove_if(records.begin(), records.end(), [&](auto& record)
			{
				if (!referencesSector(record.type)) return false;
				if (record.a == plan.sectorIndex) return true;
				if (record.a > plan.sectorIndex) --record.a;
				return false;
			}), records.end());
		}
		else
		{
			found->a = plan.y; found->b = plan.x; found->c = plan.options.cellsWide; found->i = plan.options.riseSide;
			found->x = plan.options.speed;
		}
		auto old = mSectors[plan.sectorIndex];
		int deltaX = plan.move ? (int)plan.x - (int)old->getCellX() : 0;
		int deltaY = plan.move ? (int)plan.y - (int)old->getCellY() : 0;
		rebuildFromConstructionRecords(std::move(records), plan.remove ? ~0u : plan.sectorIndex, deltaX, deltaY);
		if (plan.remove) return ~0u;
		return mLayers[layerBehind(0)]->getCellDefinition(plan.x, plan.y).sectorIndex;
	}

	bool Building::prepareStairwellEdit(StairwellEditPlan const& plan,
		vector<ConstructionRecord>& records, string& diagnostic) const
	{
		auto createsSector = [](ConstructionType type)
		{
			return type == ConstructionType::Corridor || type == ConstructionType::Room
				|| type == ConstructionType::Ladder || type == ConstructionType::Stairwell || type == ConstructionType::Staircase
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

		records = mConstructionRecords;
		uint32_t producerIndex = 0;
		auto found = records.end();
		for (auto it = records.begin(); it != records.end(); ++it)
		{
			if (!createsSector(it->type)) continue;
			if (producerIndex++ == plan.sectorIndex) { found = it; break; }
		}
		if (found == records.end() || found->type != ConstructionType::Stairwell)
		{
			diagnostic = "The selected Stairwell no longer has an authored definition";
			return false;
		}
		if (plan.remove)
		{
			records.erase(found);
			records.erase(remove_if(records.begin(), records.end(), [&](auto& record)
			{
				if (!referencesSector(record.type)) return false;
				if (record.a == plan.sectorIndex) return true;
				if (record.a > plan.sectorIndex) --record.a;
				return false;
			}), records.end());
		}
		else
		{
			found->a = plan.y; found->b = plan.x; found->c = plan.options.decksHigh;
			found->i = plan.options.mountSide; found->d = plan.options.directionalCapacity;
			found->e = plan.options.directionalBatchLimit;
		}

		try
		{
			auto candidate = makeCandidateBuilding();
			candidate->mDeserializingConstruction = true;
			vector<ConstructionRecord> viable;
			viable.reserve(records.size());
			for (auto const& record : records)
			{
				try
				{
					candidate->applyConstructionRecord(record);
					viable.push_back(record);
				}
				catch (Exception const& error)
				{
					if (createsSector(record.type))
					{
						diagnostic = error.getMessage();
						return false;
					}
					// An independently authored object made invalid by this edit is
					// intentionally removed as part of the confirmed cascade.
				}
			}
			candidate->finishBuild();
			records = std::move(viable);
		}
		catch (Exception const& error) { diagnostic = error.getMessage(); return false; }
		catch (exception const& error) { diagnostic = error.what(); return false; }
		return true;
	}

	Building::StairwellEditPlan Building::planResizeStairwell(uint32_t sectorIndex,
		uint32_t x, uint32_t y, CreateStairwellOptions const& options) const
	{
		StairwellEditPlan plan;
		plan.sectorIndex = sectorIndex; plan.x = x; plan.y = y; plan.decksHigh = options.decksHigh;
		plan.options = options;
		if (sectorIndex >= mSectors.size() || !dynamic_pointer_cast<const StairwellTransit>(mSectors[sectorIndex]))
		{ plan.diagnostic = "Only Stairwells can be edited"; return plan; }
		auto stairwell = dynamic_pointer_cast<const StairwellTransit>(mSectors[sectorIndex]);
		plan.move = x != stairwell->getCellX() || y != stairwell->getCellY();
		if (options.mountSide != CORE_SIDE_LEFT && options.mountSide != CORE_SIDE_RIGHT)
		{ plan.diagnostic = "The Stairwell mounting side is invalid"; return plan; }
		if (options.decksHigh < 2)
		{ plan.diagnostic = "A Stairwell must span at least two decks"; return plan; }
		if (options.directionalCapacity > 0 && options.directionalBatchLimit == 0)
		{ plan.diagnostic = "A constrained Stairwell requires a positive directional batch limit"; return plan; }
		if (options.directionalCapacity > 0
			&& stairwell->getAgents().size() > options.directionalCapacity)
		{ plan.diagnostic = "Directional capacity is below the current Stairwell occupancy"; return plan; }
		if (x >= mCellsWide || y >= mDecksHigh || x + 2 > mCellsWide
			|| y + options.decksHigh > mDecksHigh)
		{ plan.diagnostic = "The Stairwell is outside the Building bounds"; return plan; }
		for (uint32_t iy = y; iy < y + options.decksHigh; ++iy)
		{
			auto const& first = mLayers[0]->getCellDefinition(x, iy);
			if (first.sectorIndex == ~0u || !mSectors[first.sectorIndex]
				|| mSectors[first.sectorIndex]->getType() != SectorType::Location)
			{ plan.diagnostic = format("A Fore-layer Location is required at {},{}", x, iy); return plan; }
			for (uint32_t ix = x; ix < x + 2; ++ix)
			{
				auto const& fore = mLayers[0]->getCellDefinition(ix, iy);
				if (fore.sectorIndex != first.sectorIndex)
				{ plan.diagnostic = format("The Stairwell spans different Fore-layer Locations at deck {}", iy); return plan; }
				if (!fore.isTraversableOnFoot())
				{ plan.diagnostic = format("The Fore-layer floor at {},{} is not traversable", ix, iy); return plan; }
				auto occupant = mLayers[layerBehind(0)]->getCellDefinition(ix, iy).sectorIndex;
				if (occupant != ~0u && occupant != sectorIndex)
				{ plan.diagnostic = format("A Back-layer Sector at {},{} blocks the Stairwell", ix, iy); return plan; }
			}
		}

		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent->getSector() != stairwell.get() || plan.move) continue;
			auto position = agent->getGlobalPosition();
			if (position.y < y || position.y >= y + options.decksHigh)
				plan.consequences.push_back("Delete Agent " + agent->getName());
		}
		vector<ConstructionRecord> records;
		plan.valid = prepareStairwellEdit(plan, records, plan.diagnostic);
		if (plan.valid && records.size() < mConstructionRecords.size())
			plan.consequences.push_back(format("Delete {} dependent authored object(s)",
				mConstructionRecords.size() - records.size()));
		return plan;
	}

	Building::StairwellEditPlan Building::planRemoveStairwell(uint32_t sectorIndex) const
	{
		StairwellEditPlan plan;
		plan.remove = true; plan.sectorIndex = sectorIndex;
		if (sectorIndex >= mSectors.size() || !dynamic_pointer_cast<const StairwellTransit>(mSectors[sectorIndex]))
		{ plan.diagnostic = "Only a Stairwell can be deleted"; return plan; }
		auto stairwell = dynamic_pointer_cast<const StairwellTransit>(mSectors[sectorIndex]);
		plan.x = stairwell->getCellX(); plan.y = stairwell->getCellY();
		if (!getStairwellOptions(sectorIndex, plan.options))
		{ plan.diagnostic = "The selected Stairwell no longer has an authored definition"; return plan; }
		plan.decksHigh = plan.options.decksHigh;
		plan.consequences.push_back("Delete Stairwell");
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			if (agent->getSector() == stairwell.get())
				plan.consequences.push_back("Delete Agent " + agent->getName());
		}
		vector<ConstructionRecord> records;
		plan.valid = prepareStairwellEdit(plan, records, plan.diagnostic);
		if (plan.valid && records.size() + 1 < mConstructionRecords.size())
			plan.consequences.push_back(format("Delete {} dependent authored object(s)",
				mConstructionRecords.size() - records.size() - 1));
		return plan;
	}

	uint32_t Building::applyStairwellEdit(StairwellEditPlan const& requested)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Editing a Stairwell requires the simulation to be paused");
		auto plan = requested.remove ? planRemoveStairwell(requested.sectorIndex)
			: planResizeStairwell(requested.sectorIndex, requested.x, requested.y, requested.options);
		if (!plan.valid) throw BuildingException(this, plan.diagnostic);
		vector<ConstructionRecord> records;
		string diagnostic;
		if (!prepareStairwellEdit(plan, records, diagnostic)) throw BuildingException(this, diagnostic);
		auto old = mSectors[plan.sectorIndex];
		int deltaX = plan.remove ? 0 : (int)plan.x - (int)old->getCellX();
		int deltaY = plan.remove ? 0 : (int)plan.y - (int)old->getCellY();
		rebuildFromConstructionRecords(std::move(records),
			plan.remove ? ~0u : plan.sectorIndex, deltaX, deltaY);
		if (plan.remove) return ~0u;
		return mLayers[layerBehind(0)]->getCellDefinition(plan.x, plan.y).sectorIndex;
	}

	bool Building::prepareLocationEdit(LocationEditPlan const& plan,
		vector<ConstructionRecord>& records, uint32_t& newSectorIndex,
		string& diagnostic) const
	{
		auto createsSector = [](ConstructionType type)
		{
			return type == ConstructionType::Corridor || type == ConstructionType::Room
				|| type == ConstructionType::Ladder || type == ConstructionType::Stairwell || type == ConstructionType::Staircase
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
		auto candidate = makeCandidateBuilding();
		candidate->mDeserializingConstruction = true;
		vector<uint32_t> sectorMap(mSectors.size(), ~0u);
		uint32_t oldSectorIndex = 0;

		auto locationAt = [&](uint32_t x, uint32_t y) -> shared_ptr<const Sector>
		{
			if (x >= candidate->mCellsWide || y >= candidate->mDecksHigh) return nullptr;
			auto const& cell = candidate->mLayers[0]->getCellDefinition(x, y);
			if (cell.sectorIndex == ~0u) return nullptr;
			auto sector = candidate->getSector(cell.sectorIndex);
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
						bool any = false, all = true;
						auto doorMask = source.h ? source.h : (1u << 1);
						for (uint32_t car = 0; car < source.d; ++car)
							for (uint32_t doorOffset = 0; doorOffset < source.e; ++doorOffset)
							{
								if ((doorMask & (1u << doorOffset)) == 0) continue;
								auto doorX = source.b + offset + car * (source.e + 1) + doorOffset;
								bool supported = locationAt(doorX, source.a) != nullptr;
								any = any || supported;
								all = all && supported;
							}
						if (any && (source.p || all)) stops.push_back(offset);
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
					source.values = std::move(stops);
				}
				else if (source.type == ConstructionType::PlatformLift)
				{
					auto originalStops = source.values;
					vector<uint32_t> supported{ 0 };
					auto room = source.a < candidate->mSectors.size()
						? dynamic_pointer_cast<const Location>(candidate->mSectors[source.a]) : nullptr;
					if (room && !room->isCorridor() && source.b == 0 && source.c < room->getCellsWide())
					{
						for (size_t stop = 1; stop < originalStops.size(); ++stop)
							for (uint32_t i = 0; i < room->getNumObjects(); ++i)
							{
								auto walkway = dynamic_pointer_cast<const WalkwaySectorObject>(room->getObject(i));
								if (walkway && walkway->getCellX() == room->getCellX() + source.c
									&& walkway->getCellY() == room->getCellY() + originalStops[stop])
								{ supported.push_back(originalStops[stop]); break; }
							}
					}
					if (supported.size() < 2)
					{
						for (uint32_t slot = 0; slot < originalStops.size() + 1; ++slot)
						{
							ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
							tombstone.a = source.a;
							candidate->applyConstructionRecord(tombstone);
							records.push_back(std::move(tombstone));
						}
						continue;
					}
					source.values = std::move(supported);
				}
				else if (source.type == ConstructionType::Stairwell)
				{
					vector<uint32_t> supported;
					for (uint32_t y = source.a; y < source.a + source.c; ++y)
					{
						auto first = locationAt(source.b, y);
						if (first && locationAt(source.b + 1, y) == first) supported.push_back(y);
					}
					if (supported.size() < 2)
					{
						diagnostic = "The edit would leave a Stairwell with fewer than two supported decks. "
							"Delete the Stairwell first (transit deletion is not yet supported by the editor).";
						return false;
					}
					for (size_t i = 1; i < supported.size(); ++i)
						if (supported[i] != supported[i - 1] + 1)
						{
							diagnostic = "The edit would create an unsupported gap in a Stairwell.";
							return false;
						}
					source.a = supported.front();
					source.c = (uint32_t)supported.size();
				}

				auto const before = (uint32_t)candidate->mSectors.size();
				try
				{
					candidate->applyConstructionRecord(source);
				}
				catch (Exception const& error)
				{
					if (producer)
					{
						diagnostic = error.getMessage();
						if (source.type == ConstructionType::Ladder)
							diagnostic += " Move or delete the Ladder first.";
						return false;
					}
					if (source.type == ConstructionType::Marker
						|| source.type == ConstructionType::Walkway)
					{
						// Preserve authored object indices so a later removal cannot
						// accidentally target a different object after this one is cropped.
						ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
						tombstone.a = source.a;
						candidate->applyConstructionRecord(tombstone);
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
			candidate->finishBuild();
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

	bool Building::normalizeRoomLadderRecords(vector<ConstructionRecord>& records,
		string& diagnostic) const
	{
		struct LocationRecord { bool room{ false }; uint32_t width{ 0 }, height{ 0 }; };
		map<uint32_t, LocationRecord> locations;
		uint32_t sectorIndex = 0;
		for (auto const& record : records)
		{
			if (record.type == ConstructionType::Corridor)
				locations[sectorIndex++] = { false, record.c, record.d };
			else if (record.type == ConstructionType::Room)
				locations[sectorIndex++] = { true, record.d, record.e };
			else if (record.type == ConstructionType::Ladder
				|| record.type == ConstructionType::Stairwell || record.type == ConstructionType::Staircase || record.type == ConstructionType::Lift
				|| record.type == ConstructionType::Shuttle) ++sectorIndex;
		}
		auto hasWalkway = [&](uint32_t owner, uint32_t deck, uint32_t x)
		{
			return any_of(records.begin(), records.end(), [&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Walkway && record.a == owner
					&& record.b == deck && record.c == x;
			});
		};
		for (auto& ladder : records)
		{
			if (ladder.type != ConstructionType::SectorLadder) continue;
			auto owner = locations.find(ladder.a);
			if (owner == locations.end() || !owner->second.room)
			{
				diagnostic = "Room Ladders can only be placed in Rooms";
				return false;
			}
			if (ladder.b >= owner->second.height || ladder.c >= owner->second.width)
			{
				diagnostic = "A Room Ladder base is outside its Room";
				return false;
			}
			if (ladder.b != 0 && !hasWalkway(ladder.a, ladder.b, ladder.c))
			{
				diagnostic = "A Room Ladder must remain based on Ground or a Walkway";
				return false;
			}
			uint32_t top = ~0u;
			for (auto const& walkway : records)
				if (walkway.type == ConstructionType::Walkway && walkway.a == ladder.a
					&& walkway.c == ladder.c && walkway.b > ladder.b
					&& (top == ~0u || walkway.b < top)) top = walkway.b;
			if (top == ~0u || top >= owner->second.height)
			{
				diagnostic = "No Walkway exists above a Room Ladder";
				return false;
			}
			ladder.d = top - ladder.b + 1;
		}
		for (auto first = records.begin(); first != records.end(); ++first)
		{
			if (first->type != ConstructionType::SectorLadder) continue;
			for (auto second = next(first); second != records.end(); ++second)
			{
				if (second->type != ConstructionType::SectorLadder || first->a != second->a
					|| first->c != second->c) continue;
				uint32_t firstTop = first->b + first->d - 1;
				uint32_t secondTop = second->b + second->d - 1;
				if (max(first->b, second->b) < min(firstTop, secondTop))
				{
					diagnostic = "Room Ladder interiors cannot overlap";
					return false;
				}
			}
		}
		diagnostic.clear();
		return true;
	}

	bool Building::roomLadderIsActive(shared_ptr<const Ladder> const& ladder) const
	{
		if (!ladder) return false;
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			(void)id;
			if (resource->mLadder.get() != ladder.get()) continue;
			return !resource->mAdmissionQueue.empty()
				|| any_of(resource->mOccupants.begin(), resource->mOccupants.end(), [](AgentId id) { return (bool)id; })
				|| any_of(resource->mAdmissionReservations.begin(), resource->mAdmissionReservations.end(),
					[](TraversalRequestId id) { return (bool)id; })
				|| !resource->mExtensionRequestLeases.empty() || !resource->mExtensionOccupantLeases.empty();
		}
		return false;
	}

	bool Building::platformLiftIsActive(shared_ptr<const Lift> const& lift) const
	{
		if (!lift) return false;
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			(void)id;
			if (resource->mLift.get() != lift.get()) continue;
			return resource->mLiftMoving || !resource->mAdmissionQueue.empty()
				|| !resource->mLiftTripIntents.empty() || !resource->mLiftConfirmationQueue.empty()
				|| any_of(resource->mVirtualBoundaryOwners.begin(), resource->mVirtualBoundaryOwners.end(),
					[](auto owner) { return (bool)owner; })
				|| any_of(resource->mOccupants.begin(), resource->mOccupants.end(), [](auto owner) { return (bool)owner; })
				|| any_of(resource->mAdmissionReservations.begin(), resource->mAdmissionReservations.end(),
					[](auto owner) { return (bool)owner; });
		}
		return false;
	}

	bool Building::forceBridgeIsActive(shared_ptr<const ForceBridge> const& bridge) const
	{
		if (!bridge) return false;
		for (auto const& sector : mSectors)
		{
			if (!sector) continue;
			for (auto const* agent : sector->getAgents())
			{
				if (!agent) continue;
				auto position = agent->getGlobalPosition();
				if (position.x >= bridge->getPosition().x
					&& position.x < bridge->getPosition().x + bridge->getSize().x
					&& fabs(position.y - bridge->getPosition().y) <= 0.001f) return true;
			}
		}
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			(void)id;
			if (resource->mForceBridge.get() != bridge.get()) continue;
			auto hasOwner = [](auto const& values)
				{ return any_of(values.begin(), values.end(), [](auto value) { return (bool)value; }); };
			return !resource->mAdmissionQueue.empty() || hasOwner(resource->mOccupants)
				|| hasOwner(resource->mAdmissionReservations) || hasOwner(resource->mCrossingOwners)
				|| !resource->mExtensionRequestLeases.empty()
				|| !resource->mExtensionOccupantLeases.empty();
		}
		return false;
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
		if (object->getObjectType() == SectorObjectType::BulkheadDoor) ++sourceX;
		auto matches = [&](ConstructionRecord const& record)
		{
			switch (object->getObjectType())
			{
			case SectorObjectType::Door:
				return record.type == ConstructionType::Door
					&& record.b == sourceX && record.a == sourceY;
			case SectorObjectType::BulkheadDoor:
				return record.type == ConstructionType::BulkheadDoor
					&& record.a == owner->getLayerIndex() && record.b == sourceY
					&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u) == sourceX;
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
		if (type == SectorObjectType::Walkway && (plan.x != sourceX || plan.y != sourceY)
			&& walkwayHasOccupant(owner, sourceX, sourceY))
		{
			diagnostic = "Move the Agent standing on this Walkway before moving it";
			return false;
		}
		if (type == SectorObjectType::Walkway && (plan.x != sourceX || plan.y != sourceY))
		{
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			{
				auto ladderObject = dynamic_pointer_cast<LadderSectorObject>(owner->getObject(i));
				if (ladderObject && (ladderObject->getCellX() == sourceX
					|| ladderObject->getCellX() == plan.x) && roomLadderIsActive(ladderObject->getLadder()))
				{
					diagnostic = "A Room Ladder cannot be changed while it is in use";
					return false;
				}
				auto liftObject = dynamic_pointer_cast<LiftSectorObject>(owner->getObject(i));
				if (liftObject && liftObject->getCellX() == sourceX
					&& platformLiftIsActive(liftObject->getLift()))
				{
					diagnostic = "The connected PlatformLift is in use";
					return false;
				}
			}
		}
		if (type == SectorObjectType::Ladder
			&& roomLadderIsActive(static_pointer_cast<LadderSectorObject>(object)->getLadder()))
		{
			diagnostic = "The Room Ladder cannot be moved while it is in use";
			return false;
		}
		if (type == SectorObjectType::ForceBridge
			&& (plan.x != sourceX || plan.y != sourceY)
			&& forceBridgeIsActive(static_pointer_cast<ForceBridgeSectorObject>(object)->getForceBridge()))
		{
			diagnostic = "The Force Bridge cannot be moved while it is in use";
			return false;
		}
		bool const pastePlaced = type == SectorObjectType::Door
			|| type == SectorObjectType::BulkheadDoor || type == SectorObjectType::Window
			|| type == SectorObjectType::Marker;
		auto targetOwner = getSectorAtPosition(owner->getLayerIndex(),
			type == SectorObjectType::BulkheadDoor ? (float)plan.x - 0.5f : (float)plan.x + 0.5f,
			(float)plan.y + 0.5f);
		if (pastePlaced && !targetOwner)
		{
			diagnostic = type == SectorObjectType::Marker
				? "Markers require a viable sector" : "The destination is outside a viable sector";
			return false;
		}

		if (type == SectorObjectType::BulkheadDoor && plan.x == 0)
		{
			diagnostic = "Bulkhead Doors require a cell on each side";
			return false;
		}
		auto targetRight = type == SectorObjectType::BulkheadDoor
			? (uint64_t)plan.x + 1 : (uint64_t)plan.x + (uint32_t)ceil(object->getSize().x);
		uint64_t targetTop = type == SectorObjectType::BulkheadDoor
			? (uint64_t)plan.y + 1 : (uint64_t)plan.y + (uint32_t)ceil(object->getSize().y);
		if (type == SectorObjectType::Lift)
		{
			auto room = dynamic_pointer_cast<const Location>(owner);
			if (!room || room->isCorridor() || plan.y != room->getCellY()
				|| plan.x < room->getCellX() || plan.x >= room->getCellX() + room->getCellsWide())
			{
				diagnostic = "PlatformLifts must remain on a Room's ground floor";
				return false;
			}
			auto liftObject = static_pointer_cast<LiftSectorObject>(object);
			if (platformLiftIsActive(liftObject->getLift()))
			{
				diagnostic = "The PlatformLift cannot be moved while it is in use";
				return false;
			}
			auto candidates = getPlatformLiftStopCandidates(owner->getIndex(), plan.x - room->getCellX());
			uint32_t lowest = ~0u;
			auto layer = mLayers[room->getLayerIndex()];
			bool groundLeft = plan.x > room->getCellX0() + 1
				&& layer->getCellDefinition(plan.x - 1, plan.y).isTraversableOnFoot();
			bool groundRight = plan.x + 1 < room->getCellX1()
				&& layer->getCellDefinition(plan.x + 1, plan.y).isTraversableOnFoot();
			for (auto const& stop : candidates)
				if ((groundLeft && stop.leftButton) || (groundRight && stop.rightButton))
				{ lowest = stop.deckOffset; break; }
			if (lowest == ~0u)
			{
				diagnostic = "No eligible Walkway exists above this PlatformLift position";
				return false;
			}
			found->values = { 0, lowest };
			found->b = 0;
			found->c = plan.x - room->getCellX();
			targetTop = (uint64_t)plan.y + lowest + 1;
		}
		if (type == SectorObjectType::Ladder)
		{
			if (plan.x < owner->getCellX() || plan.y < owner->getCellY()
				|| plan.x >= owner->getCellX() + owner->getCellsWide()
				|| plan.y >= owner->getCellY() + owner->getDecksHigh())
			{
				diagnostic = "The Room Ladder must remain inside its Room";
				return false;
			}
			auto const& base = mLayers[owner->getLayerIndex()]->getCellDefinition(plan.x, plan.y);
			if (base.floorType != CellFloorType::Ground && base.floorType != CellFloorType::Walkway)
			{
				diagnostic = "Drop the Room Ladder on Ground or a Walkway";
				return false;
			}
			uint32_t top = ~0u;
			for (uint32_t iy = plan.y + 1; iy < owner->getCellY() + owner->getDecksHigh(); ++iy)
				if (mLayers[owner->getLayerIndex()]->getCellDefinition(plan.x, iy).floorType
					== CellFloorType::Walkway) { top = iy; break; }
			if (top == ~0u)
			{
				diagnostic = "No Walkway exists above this position";
				return false;
			}
			targetTop = (uint64_t)top + 1;
		}
		if (targetRight > mCellsWide || targetTop > mDecksHigh)
		{
			diagnostic = "The destination is outside the building";
			return false;
		}
		if (type == SectorObjectType::Ladder)
		{
			uint32_t topY = (uint32_t)targetTop - 1;
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			{
				auto other = owner->getObject(i);
				if (!other || other == object || other->getObjectType() == SectorObjectType::Walkway) continue;
				uint32_t right = other->getCellX() + (uint32_t)ceil(other->getSize().x) - 1;
				if (plan.x < other->getCellX() || plan.x > right) continue;
				uint32_t otherTop = other->getCellY() + (uint32_t)ceil(other->getSize().y) - 1;
				if (other->getObjectType() == SectorObjectType::Ladder)
				{
					if (max(plan.y, other->getCellY()) < min(topY, otherTop))
					{
						diagnostic = "Another Ladder overlaps this Ladder's interior";
						return false;
					}
				}
				else if (max(plan.y, other->getCellY()) <= min(topY, otherTop))
				{
					diagnostic = "Another object blocks the Ladder";
					return false;
				}
			}
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
			for (uint32_t layer = 0; layer < mLayers.size(); ++layer)
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
		case SectorObjectType::BulkheadDoor:
			found->a = owner->getLayerIndex(); found->b = plan.y; found->c = plan.x;
			found->i = CORE_SIDE_LEFT; break;
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
				for (uint32_t side = 0; side < 2; ++side)
				{
					auto sector = door->getSector(side);
					if (!sector) continue;
					owners.insert(sector->getIndex());
					ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
					tombstone.a = sector->getIndex();
					tombstones.push_back(tombstone);
					bool hasControl = side == 0 ? moved.p : moved.q;
					if (hasControl) tombstones.push_back(tombstone);
				}
			}
			else if (type == SectorObjectType::BulkheadDoor)
			{
				auto door = static_pointer_cast<BulkheadDoorSectorObject>(object)->getDoor();
				for (int side = 0; side < CORE_NUM_SIDES; ++side)
				{
					auto sector = door->getSideSector(side);
					if (!sector) continue;
					owners.insert(sector->getIndex());
					ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
					tombstone.a = sector->getIndex(); tombstones.push_back(tombstone);
					if (side == CORE_SIDE_LEFT ? moved.p : moved.q) tombstones.push_back(tombstone);
				}
			}
			else if (type == SectorObjectType::Window)
			{
				auto window = static_pointer_cast<WindowSectorObject>(object)->getWindow();
				for (uint32_t side = 0; side < 2; ++side)
					if (auto sector = window->getSector(side)) owners.insert(sector->getIndex());
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

		if (type == SectorObjectType::Walkway && (plan.x != sourceX || plan.y != sourceY))
		{
			uint32_t sourceDeck = sourceY - owner->getCellY();
			vector<ConstructionRecord> reconciled;
			for (auto const& record : records)
			{
				if (record.type != ConstructionType::PlatformLift || record.a != owner->getIndex()
					|| owner->getCellX() + record.c != sourceX
					|| find(record.values.begin(), record.values.end(), sourceDeck) == record.values.end())
				{
					reconciled.push_back(record);
					continue;
				}
				if (record.values.size() <= 2)
				{
					for (uint32_t slot = 0; slot < record.values.size() + 1; ++slot)
					{
						ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
						tombstone.a = owner->getIndex(); reconciled.push_back(std::move(tombstone));
					}
				}
				else
				{
					auto updated = record;
					updated.values.erase(remove(updated.values.begin(), updated.values.end(), sourceDeck), updated.values.end());
					reconciled.push_back(std::move(updated));
					ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
					tombstone.a = owner->getIndex(); reconciled.push_back(std::move(tombstone));
				}
			}
			records = std::move(reconciled);
			found = find_if(records.begin(), records.end(), [&](auto const& record)
			{
				return record.type == ConstructionType::Walkway && record.a == owner->getIndex()
					&& owner->getCellX() + record.c == plan.x && owner->getCellY() + record.b == plan.y;
			});
			if (found == records.end()) { diagnostic = "Could not retain the moved Walkway"; return false; }
		}

		if (!normalizeRoomLadderRecords(records, diagnostic)) return false;

		auto candidate = makeCandidateBuilding();
		candidate->mDeserializingConstruction = true;
		try
		{
			for (auto const& record : records)
			{
				auto before = newSectorIndex < candidate->mSectors.size()
					? candidate->mSectors[newSectorIndex]->getNumObjects() : 0;
				candidate->applyConstructionRecord(record);
				if (&record == &*found) newObjectIndex = before;
			}
			candidate->finishBuild();
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
		uint32_t shuttleIndex;
		if (isShuttleOwnedDoor(object, &shuttleIndex, &stopIndex))
		{
			auto plan = planRemoveShuttleStop(shuttleIndex, stopIndex);
			if (!plan.valid) throw BuildingException(this, plan.diagnostic);
			vector<ConstructionRecord> records; string diagnostic;
			if (!prepareShuttleEdit(plan, records, diagnostic)) throw BuildingException(this, diagnostic);
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
			for (uint32_t side = 0; side < 2; ++side)
			{
				auto sector = door->getSector(side);
				if (!sector) continue;
				ConstructionRecord doorTombstone{ ConstructionType::ObjectTombstone };
				doorTombstone.a = sector->getIndex();
				records.push_back(std::move(doorTombstone));
				bool hadControl = side == 0 ? source->p : source->q;
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

	bool Building::getSectorBulkheadDoorOptions(uint32_t sectorIndex, uint32_t objectIndex,
		CreateBulkheadDoorOptions& options) const
	{
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto object = dynamic_pointer_cast<const BulkheadDoorSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) return false;
		auto thresholdX = object->getCellX() + 1;
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::BulkheadDoor
					&& record.a == object->getSector()->getLayerIndex() && record.b == object->getCellY()
					&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u) == thresholdX;
			});
		if (found == mConstructionRecords.rend()) return false;
		options = { { found->p, found->q }, static_cast<DoorActivationMode>(found->j),
			found->x, found->d };
		return true;
	}

	shared_ptr<const SectorObject> Building::applySectorBulkheadDoorOptions(uint32_t sectorIndex,
		uint32_t objectIndex, CreateBulkheadDoorOptions const& options)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Editing a Bulkhead Door requires the simulation to be paused");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects())
			throw BuildingException(this, "The selected Bulkhead Door no longer exists");
		auto object = dynamic_pointer_cast<BulkheadDoorSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) throw BuildingException(this, "The selected object is not a Bulkhead Door");
		if (options.holdOpenSeconds < 0.0f)
			throw BuildingException(this, "Bulkhead Door hold-open time cannot be negative");
		if (options.crossingLanes != 1)
			throw BuildingException(this, "Bulkhead Doors support exactly one crossing lane");
		if (options.activationMode != DoorActivationMode::RemoteControlled
			&& (options.controls[0] || options.controls[1]))
			throw BuildingException(this, "Physical controls require a remote-controlled Bulkhead Door");

		auto records = mConstructionRecords;
		auto thresholdX = object->getCellX() + 1;
		auto found = find_if(records.begin(), records.end(), [&](ConstructionRecord const& record)
		{
			return record.type == ConstructionType::BulkheadDoor
				&& record.a == object->getSector()->getLayerIndex() && record.b == object->getCellY()
				&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u) == thresholdX;
		});
		if (found == records.end())
			throw BuildingException(this, "The Bulkhead Door has no authored definition");
		found->p = options.controls[0]; found->q = options.controls[1];
		found->j = static_cast<int32_t>(options.activationMode);
		found->x = options.holdOpenSeconds; found->d = options.crossingLanes;
		auto layer = object->getSector()->getLayerIndex();
		auto y = object->getCellY();
		rebuildFromConstructionRecords(std::move(records));
		auto left = getSectorAtPosition(layer, (float)thresholdX - 0.5f, (float)y + 0.5f);
		if (left)
			for (uint32_t i = 0; i < left->getNumObjects(); ++i)
			{
				auto candidate = left->getObject(i);
				if (candidate && candidate->getObjectType() == SectorObjectType::BulkheadDoor
					&& candidate->getCellX() + 1 == thresholdX && candidate->getCellY() == y)
					return candidate;
			}
		throw BuildingException(this, "Could not locate the edited Bulkhead Door");
	}

	bool Building::removeSectorBulkheadDoor(uint32_t sectorIndex, uint32_t objectIndex)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Deleting a Bulkhead Door requires the simulation to be paused");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto object = dynamic_pointer_cast<BulkheadDoorSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) return false;
		CreateBulkheadDoorOptions options;
		if (!getSectorBulkheadDoorOptions(sectorIndex, objectIndex, options)) return false;
		auto thresholdX = object->getCellX() + 1;
		auto layer = object->getSector()->getLayerIndex();
		auto y = object->getCellY();
		vector<ConstructionRecord> records;
		bool removed = false;
		for (auto const& record : mConstructionRecords)
		{
			bool const matches = !removed && record.type == ConstructionType::BulkheadDoor
				&& record.a == layer && record.b == y
				&& record.c + (record.i == CORE_SIDE_RIGHT ? 1u : 0u) == thresholdX;
			if (!matches) { records.push_back(record); continue; }
			auto door = object->getDoor();
			for (int side = 0; side < CORE_NUM_SIDES; ++side)
			{
				auto sector = door->getSideSector(side);
				if (!sector) continue;
				ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
				tombstone.a = sector->getIndex(); records.push_back(tombstone);
				if (options.controls[side]) records.push_back(tombstone);
			}
			removed = true;
		}
		if (!removed) return false;
		rebuildFromConstructionRecords(std::move(records));
		return true;
	}

	bool Building::isBulkheadDoorOwnedControl(shared_ptr<const SectorObject> const& object,
		uint32_t* doorSectorIndex, uint32_t* doorObjectIndex) const
	{
		if (!object || object->getObjectType() != SectorObjectType::InteractionPoint) return false;
		auto button = dynamic_pointer_cast<Button>(object->_getObject());
		if (!button || !button->getInteractionPointId()) return false;
		set<void const*> visited;
		for (auto const& sector : mSectors)
		{
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto doorObject = dynamic_pointer_cast<BulkheadDoorSectorObject>(sector->getObject(i));
				if (!doorObject || !visited.insert(doorObject.get()).second) continue;
				auto resource = mTraversalResources.find(doorObject->getDoor()->getTraversalResourceId());
				if (!resource || find(resource->mControls.begin(), resource->mControls.end(),
					button->getInteractionPointId()) == resource->mControls.end()) continue;
				if (doorSectorIndex) *doorSectorIndex = sector->getIndex();
				if (doorObjectIndex) *doorObjectIndex = i;
				return true;
			}
		}
		return false;
	}

	bool Building::getRoomLadderOptions(uint32_t sectorIndex, uint32_t objectIndex,
		CreateLadderOptions& options) const
	{
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto object = dynamic_pointer_cast<const LadderSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) return false;
		auto source = find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::SectorLadder && record.a == sectorIndex
					&& mSectors[sectorIndex]->getCellX() + record.c == object->getCellX()
					&& mSectors[sectorIndex]->getCellY() + record.b == object->getCellY();
			});
		if (source == mConstructionRecords.end()) return false;
		options = { source->d, source->p, source->q, source->e };
		return true;
	}

	shared_ptr<const SectorObject> Building::applyRoomLadderOptions(uint32_t sectorIndex,
		uint32_t objectIndex, CreateLadderOptions const& options)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Editing a Room Ladder requires the simulation to be paused");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects())
			throw BuildingException(this, "The selected Room Ladder no longer exists");
		auto object = dynamic_pointer_cast<LadderSectorObject>(mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) throw BuildingException(this, "The selected object is not a Room Ladder");
		if (roomLadderIsActive(object->getLadder()))
			throw BuildingException(this, "The Room Ladder cannot be edited while it is in use");
		validateSectorLadderOptions("Building::applyRoomLadderOptions", options);

		auto records = mConstructionRecords;
		auto found = find_if(records.begin(), records.end(), [&](ConstructionRecord const& record)
		{
			return record.type == ConstructionType::SectorLadder && record.a == sectorIndex
				&& mSectors[sectorIndex]->getCellX() + record.c == object->getCellX()
				&& mSectors[sectorIndex]->getCellY() + record.b == object->getCellY();
		});
		if (found == records.end()) throw BuildingException(this, "The Room Ladder has no authored definition");
		found->p = options.extensible;
		found->q = options.extensible ? options.startExtended : true;
		found->e = options.directionalBatchLimit;
		string diagnostic;
		if (!normalizeRoomLadderRecords(records, diagnostic)) throw BuildingException(this, diagnostic);
		auto x = object->getCellX(), y = object->getCellY();
		rebuildFromConstructionRecords(std::move(records));
		auto sector = _getSector(sectorIndex);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto candidate = sector->getObject(i);
			if (candidate && candidate->getObjectType() == SectorObjectType::Ladder
				&& candidate->getCellX() == x && candidate->getCellY() == y) return candidate;
		}
		throw BuildingException(this, "Could not locate the edited Room Ladder");
	}

	bool Building::removeRoomLadder(uint32_t sectorIndex, uint32_t objectIndex)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Deleting a Room Ladder requires the simulation to be paused");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto object = dynamic_pointer_cast<LadderSectorObject>(mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) return false;
		if (roomLadderIsActive(object->getLadder()))
			throw BuildingException(this, "The Room Ladder cannot be deleted while it is in use");
		CreateLadderOptions authored{};
		if (!getRoomLadderOptions(sectorIndex, objectIndex, authored)) return false;

		vector<ConstructionRecord> records;
		bool removed = false;
		for (auto const& record : mConstructionRecords)
		{
			if (!removed && record.type == ConstructionType::SectorLadder && record.a == sectorIndex
				&& mSectors[sectorIndex]->getCellX() + record.c == object->getCellX()
				&& mSectors[sectorIndex]->getCellY() + record.b == object->getCellY())
			{
				for (uint32_t i = 0; i < 3; ++i)
				{
					ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
					tombstone.a = sectorIndex;
					records.push_back(std::move(tombstone));
				}
				removed = true;
			}
			else records.push_back(record);
		}
		if (!removed) return false;
		string diagnostic;
		if (!normalizeRoomLadderRecords(records, diagnostic)) throw BuildingException(this, diagnostic);
		rebuildFromConstructionRecords(std::move(records));
		return true;
	}

	bool Building::getSectorForceBridgeOptions(uint32_t sectorIndex, uint32_t objectIndex,
		CreateForceBridgeOptions& options) const
	{
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto object = dynamic_pointer_cast<const ForceBridgeSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) return false;
		auto sector = mSectors[sectorIndex];
		auto found = find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::ForceBridge && record.a == sectorIndex
					&& sector->getCellX() + record.c == object->getCellX()
					&& sector->getCellY() + record.b == object->getCellY();
			});
		if (found == mConstructionRecords.end()) return false;
		options = { found->d, found->i, found->p, found->q, found->e };
		return true;
	}

	shared_ptr<const SectorObject> Building::applySectorForceBridgeOptions(uint32_t sectorIndex,
		uint32_t objectIndex, CreateForceBridgeOptions const& options)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Editing a Force Bridge requires the simulation to be paused");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects())
			throw BuildingException(this, "The selected Force Bridge no longer exists");
		auto object = dynamic_pointer_cast<ForceBridgeSectorObject>(mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) throw BuildingException(this, "The selected object is not a Force Bridge");
		if (forceBridgeIsActive(object->getForceBridge()))
			throw BuildingException(this, "The Force Bridge cannot be edited while it is in use");
		validateSectorForceBridgeOptions("Building::applySectorForceBridgeOptions", options);

		auto records = mConstructionRecords;
		auto sector = mSectors[sectorIndex];
		auto found = find_if(records.begin(), records.end(), [&](ConstructionRecord const& record)
		{
			return record.type == ConstructionType::ForceBridge && record.a == sectorIndex
				&& sector->getCellX() + record.c == object->getCellX()
				&& sector->getCellY() + record.b == object->getCellY();
		});
		if (found == records.end()) throw BuildingException(this, "The Force Bridge has no authored definition");
		found->d = options.width; found->i = options.fromSide; found->p = options.extensible;
		found->q = options.startExtended; found->e = options.controlCount;
		auto x = object->getCellX(), y = object->getCellY();
		rebuildFromConstructionRecords(std::move(records));
		auto rebuilt = _getSector(sectorIndex);
		for (uint32_t i = 0; i < rebuilt->getNumObjects(); ++i)
		{
			auto candidate = rebuilt->getObject(i);
			if (candidate && candidate->getObjectType() == SectorObjectType::ForceBridge
				&& candidate->getCellX() == x && candidate->getCellY() == y) return candidate;
		}
		throw BuildingException(this, "Could not locate the edited Force Bridge");
	}

	bool Building::removeSectorForceBridge(uint32_t sectorIndex, uint32_t objectIndex)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Deleting a Force Bridge requires the simulation to be paused");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto object = dynamic_pointer_cast<ForceBridgeSectorObject>(mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) return false;
		if (forceBridgeIsActive(object->getForceBridge()))
			throw BuildingException(this, "The Force Bridge cannot be deleted while it is in use");
		CreateForceBridgeOptions options;
		if (!getSectorForceBridgeOptions(sectorIndex, objectIndex, options)) return false;

		vector<ConstructionRecord> records;
		bool removed = false;
		for (auto const& record : mConstructionRecords)
		{
			if (!removed && record.type == ConstructionType::ForceBridge && record.a == sectorIndex
				&& mSectors[sectorIndex]->getCellX() + record.c == object->getCellX()
				&& mSectors[sectorIndex]->getCellY() + record.b == object->getCellY())
			{
				for (uint32_t i = 0; i < 3; ++i)
				{
					ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
					tombstone.a = sectorIndex;
					records.push_back(std::move(tombstone));
				}
				removed = true;
			}
			else records.push_back(record);
		}
		if (!removed) return false;
		rebuildFromConstructionRecords(std::move(records));
		return true;
	}

	bool Building::isForceBridgeOwnedControl(shared_ptr<const SectorObject> const& object,
		uint32_t* forceBridgeSectorIndex, uint32_t* forceBridgeObjectIndex) const
	{
		if (!object || object->getObjectType() != SectorObjectType::InteractionPoint) return false;
		auto button = dynamic_pointer_cast<Button>(object->_getObject());
		if (!button || !button->getInteractionPointId()) return false;
		for (auto const& sector : mSectors)
		{
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto bridgeObject = dynamic_pointer_cast<ForceBridgeSectorObject>(sector->getObject(i));
				if (!bridgeObject) continue;
				auto resource = mTraversalResources.find(bridgeObject->getForceBridge()->getTraversalResourceId());
				if (!resource || find(resource->mControls.begin(), resource->mControls.end(),
					button->getInteractionPointId()) == resource->mControls.end()) continue;
				if (forceBridgeSectorIndex) *forceBridgeSectorIndex = sector->getIndex();
				if (forceBridgeObjectIndex) *forceBridgeObjectIndex = i;
				return true;
			}
		}
		return false;
	}

	bool Building::getPlatformLiftOptions(uint32_t sectorIndex, uint32_t objectIndex,
		CreateLiftOptions& options) const
	{
		if (sectorIndex >= mSectors.size() || objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto object = dynamic_pointer_cast<const LiftSectorObject>(mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) return false;
		auto found = find_if(mConstructionRecords.begin(), mConstructionRecords.end(), [&](auto const& record)
		{
			return record.type == ConstructionType::PlatformLift && record.a == sectorIndex
				&& mSectors[sectorIndex]->getCellX() + record.c == object->getCellX()
				&& mSectors[sectorIndex]->getCellY() + record.b == object->getCellY();
		});
		if (found == mConstructionRecords.end()) return false;
		options.cellsWide = found->d;
		options.stopOffsets = found->values;
		options.capacity = found->e;
		options.minimumDwellSeconds = found->x;
		options.maximumBoardingSeconds = found->y;
		options.platformStopDurationSeconds = found->z;
		return true;
	}

	bool Building::preparePlatformLiftEdit(PlatformLiftEditPlan const& plan,
		vector<ConstructionRecord>& records, string& diagnostic) const
	{
		records = mConstructionRecords;
		if (plan.sectorIndex >= mSectors.size() || plan.objectIndex >= mSectors[plan.sectorIndex]->getNumObjects())
		{ diagnostic = "The selected PlatformLift no longer exists"; return false; }
		auto object = dynamic_pointer_cast<const LiftSectorObject>(mSectors[plan.sectorIndex]->getObject(plan.objectIndex));
		if (!object) { diagnostic = "The selected object is not a PlatformLift"; return false; }
		auto found = find_if(records.begin(), records.end(), [&](auto const& record)
		{
			return record.type == ConstructionType::PlatformLift && record.a == plan.sectorIndex
				&& mSectors[plan.sectorIndex]->getCellX() + record.c == object->getCellX()
				&& mSectors[plan.sectorIndex]->getCellY() + record.b == object->getCellY();
		});
		if (found == records.end()) { diagnostic = "The PlatformLift has no authored definition"; return false; }
		if (plan.remove)
		{
			auto slots = (uint32_t)found->values.size() + 1;
			auto position = (size_t)distance(records.begin(), found);
			records.erase(records.begin() + position);
			for (uint32_t slot = 0; slot < slots; ++slot)
			{
				ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
				tombstone.a = plan.sectorIndex;
				records.insert(records.begin() + position + slot, std::move(tombstone));
			}
		}
		else
		{
			found->d = plan.options.cellsWide;
			found->e = plan.options.capacity;
			found->x = plan.options.minimumDwellSeconds;
			found->y = plan.options.maximumBoardingSeconds;
			found->z = plan.options.platformStopDurationSeconds;
			found->values = plan.options.stopOffsets;
			sort(found->values.begin(), found->values.end());
			found->values.erase(unique(found->values.begin(), found->values.end()), found->values.end());
		}
		try
		{
			auto candidate = makeCandidateBuilding();
			candidate->mDeserializingConstruction = true;
			for (auto const& record : records) candidate->applyConstructionRecord(record);
			candidate->finishBuild();
		}
		catch (Exception const& error) { diagnostic = error.getMessage(); return false; }
		catch (exception const& error) { diagnostic = error.what(); return false; }
		diagnostic.clear();
		return true;
	}

	Building::PlatformLiftEditPlan Building::planPlatformLiftEdit(uint32_t sectorIndex,
		uint32_t objectIndex, CreateLiftOptions const& options) const
	{
		PlatformLiftEditPlan plan;
		plan.sectorIndex = sectorIndex; plan.objectIndex = objectIndex; plan.options = options;
		if (sectorIndex >= mSectors.size() || objectIndex >= mSectors[sectorIndex]->getNumObjects())
		{ plan.diagnostic = "The selected PlatformLift no longer exists"; return plan; }
		auto object = dynamic_pointer_cast<const LiftSectorObject>(mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) { plan.diagnostic = "The selected object is not a PlatformLift"; return plan; }
		if (platformLiftIsActive(object->getLift()))
		{ plan.diagnostic = "The PlatformLift cannot be edited while it has active journeys, queues, crossings, or reservations"; return plan; }
		CreateLiftOptions current;
		if (!getPlatformLiftOptions(sectorIndex, objectIndex, current))
		{ plan.diagnostic = "The PlatformLift has no authored definition"; return plan; }
		auto desired = options.stopOffsets;
		sort(desired.begin(), desired.end()); desired.erase(unique(desired.begin(), desired.end()), desired.end());
		if (desired.size() < 2 || desired.front() != 0)
		{ plan.diagnostic = "A PlatformLift requires ground and at least one Walkway stop"; return plan; }
		plan.options.stopOffsets = desired;
		for (auto stop : current.stopOffsets)
			if (find(desired.begin(), desired.end(), stop) == desired.end())
				plan.consequences.push_back(format("Remove PlatformLift landing, button, pathing, and stop at deck {}", stop));
		auto room = mSectors[sectorIndex];
		auto candidates = getPlatformLiftStopCandidates(sectorIndex,
			object->getCellX() - room->getCellX());
		auto buttonSide = [&](vector<uint32_t> const& stops)
		{
			auto layer = mLayers[room->getLayerIndex()];
			auto x = object->getCellX();
			bool left = x > room->getCellX0() + 1
				&& layer->getCellDefinition(x - 1, room->getCellY()).isTraversableOnFoot();
			bool right = x + 1 < room->getCellX1()
				&& layer->getCellDefinition(x + 1, room->getCellY()).isTraversableOnFoot();
			for (size_t i = 1; i < stops.size(); ++i)
			{
				auto found = find_if(candidates.begin(), candidates.end(), [&](auto const& value)
					{ return value.deckOffset == stops[i]; });
				left = left && found != candidates.end() && found->leftButton;
				right = right && found != candidates.end() && found->rightButton;
			}
			return right ? CORE_SIDE_RIGHT : left ? CORE_SIDE_LEFT : CORE_SIDE_MIDDLE;
		};
		auto oldSide = buttonSide(current.stopOffsets), newSide = buttonSide(desired);
		if (oldSide != newSide && newSide != CORE_SIDE_MIDDLE)
			plan.consequences.push_back(format("Move all PlatformLift landing buttons to the {} side",
				newSide == CORE_SIDE_LEFT ? "left" : "right"));
		vector<ConstructionRecord> records;
		plan.valid = preparePlatformLiftEdit(plan, records, plan.diagnostic);
		return plan;
	}

	Building::PlatformLiftEditPlan Building::planRemovePlatformLift(uint32_t sectorIndex,
		uint32_t objectIndex) const
	{
		CreateLiftOptions options;
		auto plan = planPlatformLiftEdit(sectorIndex, objectIndex,
			getPlatformLiftOptions(sectorIndex, objectIndex, options) ? options : CreateLiftOptions{});
		if (!plan.valid) return plan;
		plan.remove = true;
		plan.consequences.clear();
		vector<ConstructionRecord> records;
		plan.valid = preparePlatformLiftEdit(plan, records, plan.diagnostic);
		return plan;
	}

	shared_ptr<const SectorObject> Building::applyPlatformLiftEdit(PlatformLiftEditPlan const& requested)
	{
		if (!mSimulationPaused) throw BuildingException(this, "Editing a PlatformLift requires the simulation to be paused");
		auto plan = requested.remove ? planRemovePlatformLift(requested.sectorIndex, requested.objectIndex)
			: planPlatformLiftEdit(requested.sectorIndex, requested.objectIndex, requested.options);
		if (!plan.valid) throw BuildingException(this, plan.diagnostic);
		vector<ConstructionRecord> records; string diagnostic;
		if (!preparePlatformLiftEdit(plan, records, diagnostic)) throw BuildingException(this, diagnostic);
		auto room = mSectors[plan.sectorIndex];
		auto x = room->getCellX();
		if (plan.objectIndex < room->getNumObjects() && room->getObject(plan.objectIndex))
			x = room->getObject(plan.objectIndex)->getCellX();
		rebuildFromConstructionRecords(std::move(records));
		if (plan.remove) return nullptr;
		room = mSectors[plan.sectorIndex];
		for (uint32_t i = 0; i < room->getNumObjects(); ++i)
		{
			auto object = room->getObject(i);
			if (object && object->getObjectType() == SectorObjectType::Lift && object->getCellX() == x)
				return object;
		}
		throw BuildingException(this, "Could not locate the edited PlatformLift");
	}

	Building::WalkwayEditPlan Building::planRemoveSectorWalkway(uint32_t sectorIndex,
		uint32_t objectIndex) const
	{
		WalkwayEditPlan plan;
		plan.sectorIndex = sectorIndex; plan.objectIndex = objectIndex;
		if (sectorIndex >= mSectors.size() || objectIndex >= mSectors[sectorIndex]->getNumObjects()
			|| !dynamic_pointer_cast<const WalkwaySectorObject>(mSectors[sectorIndex]->getObject(objectIndex)))
		{ plan.diagnostic = "The selected Walkway no longer exists"; return plan; }
		auto walkway = mSectors[sectorIndex]->getObject(objectIndex);
		for (uint32_t i = 0; i < mSectors[sectorIndex]->getNumObjects(); ++i)
		{
			auto liftObject = dynamic_pointer_cast<const LiftSectorObject>(mSectors[sectorIndex]->getObject(i));
			if (!liftObject || liftObject->getCellX() != walkway->getCellX()) continue;
			CreateLiftOptions options;
			if (!getPlatformLiftOptions(sectorIndex, i, options)) continue;
			auto deck = walkway->getCellY() - mSectors[sectorIndex]->getCellY();
			if (find(options.stopOffsets.begin(), options.stopOffsets.end(), deck) == options.stopOffsets.end()) continue;
			if (platformLiftIsActive(liftObject->getLift()))
			{ plan.diagnostic = "The connected PlatformLift is in use"; return plan; }
			if (options.stopOffsets.size() <= 2)
				plan.consequences.push_back("Delete connected PlatformLift");
		}
		plan.valid = true;
		return plan;
	}

	bool Building::applyWalkwayEdit(WalkwayEditPlan const& plan)
	{
		if (!plan.valid) throw BuildingException(this, plan.diagnostic);
		return removeSectorWalkway(plan.sectorIndex, plan.objectIndex);
	}

	bool Building::removeSectorWalkway(uint32_t sectorIndex, uint32_t objectIndex)
	{
		if (!mSimulationPaused)
			throw BuildingException(this, "Deleting a Walkway requires the simulation to be paused");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects()) return false;
		auto object = dynamic_pointer_cast<WalkwaySectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (!object) return false;
		if (walkwayHasOccupant(object->getSector(), object->getCellX(), object->getCellY()))
			throw BuildingException(this, "Move the Agent standing on this Walkway before deleting it");
		for (uint32_t i = 0; i < object->getSector()->getNumObjects(); ++i)
		{
			auto ladderObject = dynamic_pointer_cast<LadderSectorObject>(object->getSector()->getObject(i));
			if (!ladderObject || ladderObject->getCellX() != object->getCellX()) continue;
			auto ladder = ladderObject->getLadder();
			uint32_t top = ladderObject->getCellY() + ladder->getDecksHigh() - 1;
			if ((ladderObject->getCellY() == object->getCellY() || top == object->getCellY())
				&& roomLadderIsActive(ladder))
				throw BuildingException(this, "A Room Ladder cannot be changed while it is in use");
		}

		auto source = find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Walkway && record.a == sectorIndex
					&& mSectors[sectorIndex]->getCellX() + record.c == object->getCellX()
					&& mSectors[sectorIndex]->getCellY() + record.b == object->getCellY();
			});
		if (source == mConstructionRecords.end()) return false;

		auto room = mSectors[sectorIndex];
		auto layer = mLayers[room->getLayerIndex()];
		uint32_t supportX = object->getCellX(), supportY = object->getCellY();
		uint32_t supportDeck = supportY - room->getCellY();
		map<size_t, ConstructionRecord> platformUpdates;
		set<size_t> platformDeletions;
		for (size_t recordIndex = 0; recordIndex < mConstructionRecords.size(); ++recordIndex)
		{
			auto const& record = mConstructionRecords[recordIndex];
			if (record.type != ConstructionType::PlatformLift || record.a != sectorIndex
				|| room->getCellX() + record.c != supportX
				|| find(record.values.begin(), record.values.end(), supportDeck) == record.values.end()) continue;
			shared_ptr<const LiftSectorObject> liftObject;
			for (uint32_t i = 0; i < room->getNumObjects(); ++i)
			{
				auto candidate = dynamic_pointer_cast<const LiftSectorObject>(room->getObject(i));
				if (candidate && candidate->getCellX() == supportX) { liftObject = candidate; break; }
			}
			if (liftObject && platformLiftIsActive(liftObject->getLift()))
				throw BuildingException(this, "The connected PlatformLift is in use");
			if (record.values.size() <= 2) platformDeletions.insert(recordIndex);
			else
			{
				auto updated = record;
				updated.values.erase(remove(updated.values.begin(), updated.values.end(), supportDeck), updated.values.end());
				platformUpdates.emplace(recordIndex, std::move(updated));
			}
		}
		map<size_t, ConstructionRecord> bridgeUpdates;
		for (size_t recordIndex = 0; recordIndex < mConstructionRecords.size(); ++recordIndex)
		{
			auto const& record = mConstructionRecords[recordIndex];
			if (record.type != ConstructionType::ForceBridge || record.a != sectorIndex
				|| room->getCellY() + record.b != supportY) continue;
			uint32_t bridgeX = room->getCellX() + record.c;
			uint32_t bridgeRightSupport = bridgeX + record.d;
			uint32_t originSupport = record.i == CORE_SIDE_LEFT ? bridgeX - 1 : bridgeRightSupport;
			uint32_t destinationSupport = record.i == CORE_SIDE_LEFT ? bridgeRightSupport : bridgeX - 1;
			if (supportX == originSupport)
				throw BuildingException(this,
					"Cannot delete the Walkway on the side from which a Force Bridge extends");
			if (supportX != destinationSupport) continue;

			shared_ptr<ForceBridgeSectorObject> bridgeObject;
			for (uint32_t i = 0; i < room->getNumObjects(); ++i)
			{
				auto candidate = dynamic_pointer_cast<ForceBridgeSectorObject>(room->getObject(i));
				if (candidate && candidate->getCellX() == bridgeX
					&& candidate->getCellY() == supportY) { bridgeObject = candidate; break; }
			}
			if (bridgeObject && forceBridgeIsActive(bridgeObject->getForceBridge()))
				throw BuildingException(this, "The Force Bridge cannot be resized while it is in use");

			auto updated = record;
			bool foundWalkway = false;
			if (record.i == CORE_SIDE_LEFT)
			{
				uint32_t roomRight = room->getCellX() + room->getCellsWide();
				for (uint32_t x = supportX + 1; x < roomRight; ++x)
				{
					auto floor = layer->getCellDefinition(x, supportY).floorType;
					if (floor == CellFloorType::Walkway)
					{
						updated.d = x - bridgeX;
						foundWalkway = true;
						break;
					}
					if (floor != CellFloorType::None)
						throw BuildingException(this,
							"Another floor object blocks the Force Bridge before the next Walkway");
				}
			}
			else
			{
				for (int64_t x = (int64_t)supportX - 1; x >= (int64_t)room->getCellX(); --x)
				{
					auto floor = layer->getCellDefinition((uint32_t)x, supportY).floorType;
					if (floor == CellFloorType::Walkway)
					{
						uint32_t newBridgeX = (uint32_t)x + 1;
						updated.c = newBridgeX - room->getCellX();
						updated.d = bridgeRightSupport - newBridgeX;
						foundWalkway = true;
						break;
					}
					if (floor != CellFloorType::None)
						throw BuildingException(this,
							"Another floor object blocks the Force Bridge before the next Walkway");
				}
			}
			if (!foundWalkway)
				throw BuildingException(this,
					"Cannot delete this Walkway because no replacement Walkway supports the Force Bridge");
			if (updated.d == 0 || updated.d > CORE_FORCEBRIDGE_MAX_SIZE)
				throw BuildingException(this, format(
					"The next Walkway is farther than the maximum Force Bridge width of {}",
					CORE_FORCEBRIDGE_MAX_SIZE));
			bridgeUpdates.emplace(recordIndex, std::move(updated));
		}

		vector<ConstructionRecord> records;
		vector<ConstructionRecord> movedBridges;
		records.reserve(mConstructionRecords.size() + bridgeUpdates.size() * 3);
		for (size_t i = 0; i < mConstructionRecords.size(); ++i)
		{
			if (&mConstructionRecords[i] == &*source)
			{
				ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
				tombstone.a = sectorIndex;
				records.push_back(std::move(tombstone));
			}
			else if (platformDeletions.contains(i))
			{
				for (uint32_t slot = 0; slot < mConstructionRecords[i].values.size() + 1; ++slot)
				{
					ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
					tombstone.a = sectorIndex;
					records.push_back(std::move(tombstone));
				}
			}
			else if (auto update = platformUpdates.find(i); update != platformUpdates.end())
			{
				records.push_back(update->second);
				ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
				tombstone.a = sectorIndex;
				records.push_back(std::move(tombstone));
			}
			else if (auto update = bridgeUpdates.find(i); update != bridgeUpdates.end())
			{
				for (uint32_t slot = 0; slot < 3; ++slot)
				{
					ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
					tombstone.a = sectorIndex;
					records.push_back(std::move(tombstone));
				}
				movedBridges.push_back(update->second);
			}
			else records.push_back(mConstructionRecords[i]);
		}
		records.insert(records.end(), movedBridges.begin(), movedBridges.end());
		rebuildFromConstructionRecords(std::move(records));
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
			for (uint32_t side = 0; side < 2; ++side)
				if (auto sector = window->getSector(side)) sectorIndices.insert(sector->getIndex());
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
		if (sectorIndex < mSectors.size() && objectIndex < mSectors[sectorIndex]->getNumObjects())
		{
			auto object = mSectors[sectorIndex]->getObject(objectIndex);
			if (object)
			{
				plan.previewWidth = (uint32_t)ceil(object->getSize().x);
				plan.previewHeight = (uint32_t)ceil(object->getSize().y);
				if (plan.valid && object->getObjectType() == SectorObjectType::Ladder)
					for (auto const& record : records)
						if (record.type == ConstructionType::SectorLadder && record.a == sectorIndex
							&& mSectors[sectorIndex]->getCellX() + record.c == x
							&& mSectors[sectorIndex]->getCellY() + record.b == y)
						{ plan.previewHeight = record.d; break; }
				if (plan.valid && object->getObjectType() == SectorObjectType::Lift
					&& (x != object->getCellX() || y != object->getCellY()))
				{
					for (auto const& record : records)
						if (record.type == ConstructionType::PlatformLift && record.a == sectorIndex
							&& mSectors[sectorIndex]->getCellX() + record.c == x)
						{
							plan.previewHeight = record.values.back() + 1;
							plan.consequences = {
								"Move PlatformLift and rebuild its landing buttons",
								"Reset PlatformLift stops to ground and the lowest eligible Walkway" };
							break;
						}
				}
				if (plan.valid && object->getObjectType() == SectorObjectType::Walkway
					&& (x != object->getCellX() || y != object->getCellY()))
				{
					uint32_t sourceDeck = object->getCellY() - object->getSector()->getCellY();
					for (auto const& record : mConstructionRecords)
						if (record.type == ConstructionType::PlatformLift && record.a == sectorIndex
							&& mSectors[sectorIndex]->getCellX() + record.c == object->getCellX()
							&& find(record.values.begin(), record.values.end(), sourceDeck) != record.values.end())
							if (record.values.size() <= 2)
								plan.consequences.push_back("Delete connected PlatformLift");
				}
			}
		}
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
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto liftObject = dynamic_pointer_cast<const LiftSectorObject>(sector->getObject(i));
			if (liftObject && platformLiftIsActive(liftObject->getLift()))
			{
				plan.diagnostic = "A Room cannot be edited while its PlatformLift is in use";
				return plan;
			}
		}
		auto sectorId = SectorId{ (uint64_t)sectorIndex + 1 };
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			if (!resource->mShuttle || none_of(resource->mShuttleDoors.begin(), resource->mShuttleDoors.end(),
				[sectorId](auto const& door) { return door.locationSector == sectorId; })) continue;
			bool active = resource->mLiftMoving || !resource->mAdmissionQueue.empty()
				|| !resource->mLiftTripIntents.empty() || !resource->mLiftConfirmationQueue.empty()
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
			{
				plan.diagnostic = "A platform cannot be edited while its Shuttle has active journeys, queues, crossings, or reservations";
				return plan;
			}
		}
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
			bool const walkway = object->getObjectType() == SectorObjectType::Walkway;
			auto const relativeX = object->getCellX() - sector->getCellX();
			auto const relativeY = object->getCellY() - sector->getCellY();
			bool walkwayCropped = walkway && (relativeX >= cellsWide || relativeY >= decksHigh);
			bool walkwayMoved = walkway && !walkwayCropped
				&& (x + relativeX != object->getCellX() || y + relativeY != object->getCellY());
			if ((walkwayCropped || walkwayMoved)
				&& walkwayHasOccupant(sector, object->getCellX(), object->getCellY()))
			{
				plan.diagnostic = "Move the Agent standing on the affected Walkway before resizing the Room";
				return plan;
			}
			if (!inside || losesFloor || walkwayCropped)
				plan.consequences.push_back("Delete " + object->getDescription());
		}
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto liftObject = dynamic_pointer_cast<const LiftSectorObject>(sector->getObject(i));
			if (!liftObject) continue;
			CreateLiftOptions options;
			if (!getPlatformLiftOptions(sectorIndex, i, options)) continue;
			uint32_t retained = 1;
			for (size_t stop = 1; stop < options.stopOffsets.size(); ++stop)
			{
				bool walkwayRetained = false;
				for (uint32_t j = 0; j < sector->getNumObjects(); ++j)
				{
					auto walkway = dynamic_pointer_cast<const WalkwaySectorObject>(sector->getObject(j));
					if (!walkway || walkway->getCellX() != liftObject->getCellX()
						|| walkway->getCellY() != sector->getCellY() + options.stopOffsets[stop]) continue;
					auto relativeX = walkway->getCellX() - sector->getCellX();
					auto relativeY = walkway->getCellY() - sector->getCellY();
					walkwayRetained = plan.move || (relativeX < cellsWide && relativeY < decksHigh);
					break;
				}
				if (walkwayRetained) ++retained;
				else plan.consequences.push_back(format("Remove PlatformLift stop at deck {}", options.stopOffsets[stop]));
			}
			if (!plan.move && (liftObject->getCellX() - sector->getCellX() >= cellsWide || retained < 2))
				plan.consequences.push_back("Delete Platform Lift");
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
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			(void)id;
			if (!resource->mShuttle) continue;
			set<uint32_t> affectedStops;
			for (auto const& door : resource->mShuttleDoors)
				if (door.locationSector == sectorId) affectedStops.insert(door.stopIndex);
			for (auto stop : affectedStops)
				plan.consequences.push_back(format("Reconcile Shuttle stop {} carriage landings", stop));
		}
		for (auto const& candidateSector : mSectors)
		{
			auto transit = dynamic_pointer_cast<Transit const>(candidateSector);
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
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto liftObject = dynamic_pointer_cast<const LiftSectorObject>(sector->getObject(i));
			if (liftObject && platformLiftIsActive(liftObject->getLift()))
			{
				plan.diagnostic = "A Room cannot be deleted while its PlatformLift is in use";
				return plan;
			}
		}
		auto sectorId = SectorId{ (uint64_t)sectorIndex + 1 };
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			if (!resource->mShuttle || none_of(resource->mShuttleDoors.begin(), resource->mShuttleDoors.end(),
				[sectorId](auto const& door) { return door.locationSector == sectorId; })) continue;
			bool active = resource->mLiftMoving || !resource->mAdmissionQueue.empty()
				|| !resource->mLiftTripIntents.empty() || !resource->mLiftConfirmationQueue.empty()
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
			{
				plan.diagnostic = "A platform cannot be deleted while its Shuttle has active journeys, queues, crossings, or reservations";
				return plan;
			}
		}
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
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			(void)id;
			if (!resource->mShuttle) continue;
			set<uint32_t> affectedStops;
			for (auto const& door : resource->mShuttleDoors)
				if (door.locationSector == sectorId) affectedStops.insert(door.stopIndex);
			for (auto stop : affectedStops)
				plan.consequences.push_back(format("Remove Shuttle stop {} carriage landing", stop));
		}
		for (auto const& candidateSector : mSectors)
		{
			auto transit = dynamic_pointer_cast<Transit const>(candidateSector);
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
