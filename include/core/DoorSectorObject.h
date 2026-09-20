#pragma once

#include <cstdint>
#include <memory>

#include "core/Door.h"
#include "core/SectorObject.h"


namespace core
{

	class DoorSectorObject : public SectorObject
	{
		std::vector<float> calculateDoorQueueStopOffsets(Building const* building, std::shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t doorWidth) const;

	public:

		DoorSectorObject(uint32_t cellX, uint32_t cellY, uint32_t cellsWide,
			std::shared_ptr<const Sector> sectors[2], uint32_t* vertexIdentifer = nullptr,
			Door::Height height = Door::Height::Regular);

		~DoorSectorObject() = default;

		std::shared_ptr<Door> getDoor() const;

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};

} // core
