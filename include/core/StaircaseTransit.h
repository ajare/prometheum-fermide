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
		StaircaseTransit(uint32_t index, uint32_t layerIndex, uint32_t cellX, uint32_t cellY,
			uint32_t cellsWide, int riseSide, float speed,
			std::vector<TransitStop> const& stops);
		StaircaseTransit(uint32_t index, uint32_t layerIndex, uint32_t cellX, uint32_t cellY,
			uint32_t cellsWide, int riseSide, std::vector<TransitStop> const& stops)
			: StaircaseTransit(index, layerIndex, cellX, cellY, cellsWide, riseSide, 0.0f, stops) {}
		~StaircaseTransit() = default;

		[[nodiscard]] std::shared_ptr<Staircase> getStaircase() const { return mStaircase; }
		[[nodiscard]] int getRiseSide() const { return mStaircase->getRiseSide(); }
		[[nodiscard]] std::string getDescription() const override;
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;
		[[nodiscard]] std::shared_ptr<Edge> createCrossLevelEdge(
			std::shared_ptr<VerticalEdgeCreator> edgeCreator) const override;

		void updateImpl(float frameTime) override;
	};
}
