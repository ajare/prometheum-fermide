#pragma once

#include <memory>
#include <vector>

#include "core/Staircase.h"
#include "core/Transit.h"
#include "core/VerticalEdgeCreator.h"

namespace core
{
	class StaircaseTransit : public Transit, public VerticalEdgeCreator
	{
		std::shared_ptr<Staircase> mStaircase;

	public:
		StaircaseTransit(uint32_t index, uint32_t cellX, uint32_t cellY,
			uint32_t cellsWide, int riseSide, std::vector<TransitStop> const& stops);
		~StaircaseTransit() = default;

		[[nodiscard]] std::shared_ptr<Staircase> getStaircase() const { return mStaircase; }
		[[nodiscard]] int getRiseSide() const { return mStaircase->getRiseSide(); }
		[[nodiscard]] std::string getDescription() const override;
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;
		[[nodiscard]] std::shared_ptr<Edge> createCrossDeckEdge(
			std::shared_ptr<VerticalEdgeCreator> edgeCreator) const override;
	};
}
