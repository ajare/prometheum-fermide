#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/SectorObjectType.h"


namespace core
{
	enum struct CellFloorType
	{
		None,
		Ground,
		Walkway,
		ForceBridge
	};

	std::string getCellFloorTypeString(CellFloorType type);

	struct CellDefinition
	{
		// Index of Sector within the owning Building.  -1 means no Sector.
		uint32_t sectorIndex{ ~0u };

		SectorObjectType sectorObjectType{ SectorObjectType::None };
		
		uint32_t sectorObjectIndex{ ~0u };

		// Cannot have more than 3 - 1 per side
		uint32_t controls[3] = { ~0u, ~0u, ~0u };

		// Index of Bulkhead doors on either side.
		uint32_t bulkheadIndices[2] = { ~0u, ~0u };

		// Floor type
		uint32_t floorIndex{ ~0u };
		CellFloorType floorType{ CellFloorType::Ground };

		// Markers
		std::vector<uint32_t> markers;

	public:

		bool occupied() const
		{
			return sectorIndex != ~0u;
		}

		bool hasObject() const
		{
			return sectorObjectType != SectorObjectType::None;
		}

		bool isTraversableOnFoot() const
		{
			return floorType != CellFloorType::None;
		}

	};

} // core
