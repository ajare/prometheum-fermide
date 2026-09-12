#pragma once

#include <vector>

#include "core/Transit.h"
#include "core/VerticalEdgeCreator.h"
#include "core/Staircase.h"


namespace core
{
	class StaircaseTransit : public Transit, public VerticalEdgeCreator
	{
		std::shared_ptr<Staircase> mStaircase;

		int mMountSide;

	public:

		StaircaseTransit(uint32_t index, uint32_t cellX, uint32_t cellY, uint32_t decksHigh, int mountSide, std::vector<TransitStop> const& stops);

		~StaircaseTransit() = default;

		std::shared_ptr<Staircase> getStaircase() const;

		int getMountSide() const;

		// Overridden from Transit
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Trasnit
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;

		// Overridden from VerticalEdgeCreator
		[[nodiscard]] std::shared_ptr<Edge> createCrossDeckEdge(std::shared_ptr<VerticalEdgeCreator> edgeCreator) const override;
	};

} // core

