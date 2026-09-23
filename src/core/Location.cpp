#include <format>
#include <cassert>

#include "core/Defines.h"
#include "core/Location.h"
#include "core/DoorSectorObject.h"
#include "core/WindowSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/ButtonSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"


namespace core
{

	using namespace std;

	Location::Location(string const& name, SectorType type, uint32_t layerIndex, uint32_t index, uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, uint32_t capacity, bool isCorridor)
		: Sector(type, layerIndex, index, cellX, cellY, 0.0f, 0.0f, (float)cellsWide, (float)((levelsHigh - 1) + topLevelHeight), name, cellsWide, levelsHigh, topLevelHeight, capacity)
		, mIsCorridor(isCorridor)
	{
	}

	string Location::getDescription() const
	{
		return format("{} at {},{} on Layer {}", getName(), getCellX(), getCellY(), getLayerIndex());
	}

	bool Location::sectorSupportsObjectType(SectorObjectType type) const
	{
		return type == SectorObjectType::BulkheadDoor ||
			type == SectorObjectType::Door ||
			type == SectorObjectType::ForceBridge ||
			type == SectorObjectType::InteractionPoint ||
			type == SectorObjectType::Ladder ||
			type == SectorObjectType::Lift ||
			type == SectorObjectType::Marker ||
			type == SectorObjectType::Walkway ||
			type == SectorObjectType::Window;
	}

} // core