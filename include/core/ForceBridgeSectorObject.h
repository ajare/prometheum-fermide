#pragma once

#include <cstdint>
#include <memory>

#include "core/ForceBridge.h"
#include "core/SectorObject.h"


namespace core
{

	class ForceBridgeSectorObject : public SectorObject
	{
	public:

		ForceBridgeSectorObject(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, int fromSide, std::shared_ptr<const Sector> sector, bool extensible, bool startExtended);

		~ForceBridgeSectorObject() = default;

		std::shared_ptr<ForceBridge> getForceBridge() const;

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};

} // core
