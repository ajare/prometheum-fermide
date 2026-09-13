#pragma once

#include <cstdint>
#include <memory>

#include "core/BulkheadDoor.h"
#include "core/SectorObject.h"


namespace core
{

	class BulkheadDoorSectorObject : public SectorObject
	{
	public:

		BulkheadDoorSectorObject(uint32_t cellX, uint32_t cellY, std::shared_ptr<const Sector> sectors[2]);

		~BulkheadDoorSectorObject() = default;

		std::shared_ptr<BulkheadDoor> getDoor() const;

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};

} // core
