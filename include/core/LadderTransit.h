#pragma once

#include "core/Transit.h"
#include "core/VerticalEdgeCreator.h"
#include "core/Ladder.h"


namespace core
{
	class LadderTransit : public Transit, public VerticalEdgeCreator
	{
		std::shared_ptr<Ladder> mLadder;

	private:

		// Overridden from Transit
		void updateImpl(float frameTime) override;

	public:

		LadderTransit(uint32_t index, uint32_t layerIndex, uint32_t cellX, uint32_t cellY, uint32_t decksHigh, std::vector<TransitStop> const& stops, bool extensible, bool startExtended);

		~LadderTransit() = default;

		std::shared_ptr<Ladder> getLadder() const;

		// Overridden from Transit
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Trasnit
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;

		// Overridden from VerticalEdgeCreator
		[[nodiscard]] std::shared_ptr<Edge> createCrossDeckEdge(std::shared_ptr<VerticalEdgeCreator> edgeCreator) const override;
	};

} // core

