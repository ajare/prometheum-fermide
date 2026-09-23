#pragma once

#include <vector>

#include "core/Transit.h"
#include "core/VerticalEdgeCreator.h"
#include "core/Stairwell.h"


namespace core
{
	class StairwellTransit : public Transit, public VerticalEdgeCreator
	{
		std::shared_ptr<Stairwell> mStairwell;

		int mMountSide;

	public:

		StairwellTransit(uint32_t index, uint32_t layerIndex, uint32_t cellX, uint32_t cellY, uint32_t levelsHigh, int mountSide, std::vector<TransitStop> const& stops);

		~StairwellTransit() = default;

		std::shared_ptr<Stairwell> getStairwell() const;

		int getMountSide() const;

		// Overridden from Transit
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Trasnit
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;

		// Overridden from VerticalEdgeCreator
		[[nodiscard]] std::shared_ptr<Edge> createCrossLevelEdge(std::shared_ptr<VerticalEdgeCreator> edgeCreator) const override;
	};

} // core

