#pragma once

#include <cstdint>
#include <memory>

#include "core/Ladder.h"
#include "core/SectorObject.h"


namespace core
{

	class LadderSectorObject : public SectorObject, public VerticalEdgeCreator
	{
	public:

		LadderSectorObject(uint32_t cellX, uint32_t cellY, uint32_t levelsHigh, std::shared_ptr<const Sector> sector, bool extensible, bool startExtended, uint32_t* vertexIdentifer = nullptr);

		~LadderSectorObject() = default;

		std::shared_ptr<Ladder> getLadder() const;

		// Overridden from VerticalEdgeCreator
		[[nodiscard]] std::shared_ptr<Edge> createCrossLevelEdge(std::shared_ptr<VerticalEdgeCreator> edgeCreator) const override;

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};

} // core
