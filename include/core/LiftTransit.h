#pragma once

#include <vector>

#include "core/Transit.h"
#include "core/VerticalEdgeCreator.h"
#include "core/Lift.h"


namespace core
{
	class LiftTransit : public Transit, public VerticalEdgeCreator
	{
		std::shared_ptr<Lift> mLift;

	private:

		// Overridden from Transit
		void updateImpl(float frameTime) override;

	public:

		LiftTransit(uint32_t index, uint32_t layerIndex, uint32_t cellX, uint32_t cellY,
			uint32_t cellsWide, uint32_t decksHigh, std::vector<TransitStop> const& stops);

		~LiftTransit() = default;

		std::shared_ptr<Lift> getLift() const;

		// Overridden from Transit
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Transit
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;

		// Overridden from VerticalEdgeCreator
		[[nodiscard]] std::shared_ptr<Edge> createCrossDeckEdge(std::shared_ptr<VerticalEdgeCreator> edgeCreator) const override;
	};

} // core

