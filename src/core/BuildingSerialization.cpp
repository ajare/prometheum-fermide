#include "core/Building.h"
#include "core/SerializationException.h"
#include "core/Exceptions.h"
#include "core/Transit.h"

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
			if (kind > static_cast<uint32_t>(ConstructionType::ObjectTombstone))
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
				{ record.c, record.values, record.d, record.x, record.y, record.g });
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
						auto first = locationAt(source.b, y);
						bool supported = first != nullptr;
						for (uint32_t x = source.b; supported && x < source.b + source.c; ++x)
							supported = locationAt(x, y) == first;
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
		if (cellsWide == 0 || decksHigh == 0 || x + cellsWide >= mCellsWide || y + decksHigh >= mDecksHigh)
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
