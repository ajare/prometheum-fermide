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
		[[nodiscard]] std::shared_ptr<VertexController> createVertexController(Building const* building, std::vector<std::shared_ptr<Vertex>> const& vertices, std::map<std::shared_ptr<Controller>, std::shared_ptr<Vertex>> const& controllerVertexLookup) const override;

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};

} // core
