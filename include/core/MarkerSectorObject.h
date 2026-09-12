#pragma once

#include <cstdint>
#include <memory>

#include "core/Marker.h"
#include "core/SectorObject.h"


namespace core
{

	class MarkerSectorObject : public SectorObject
	{
	public:

		MarkerSectorObject(uint32_t cellX, uint32_t cellY, std::shared_ptr<const Sector> sector, float xOffset, uint32_t* vertexIdentifer = nullptr);

		~MarkerSectorObject() = default;

		std::shared_ptr<const Marker> getMarker() const;

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};

} // core
