#pragma once

#include <vector>

#include "core/Transit.h"
#include "core/VerticalEdgeCreator.h"
#include "core/Shuttle.h"


namespace core
{
	class ShuttleTransit : public Transit, public VerticalEdgeCreator
	{
		std::shared_ptr<Shuttle> mShuttle;

	private:

		// Overridden from Transit
		void updateImpl(float frameTime) override;

	public:

		ShuttleTransit(uint32_t index, uint32_t layerIndex, uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t numCars, uint32_t carWidth, std::vector<TransitStop> const& stops);

		~ShuttleTransit() = default;

		std::shared_ptr<Shuttle> getShuttle() const;

		// Overridden from Transit
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Transit
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;

		// Overridden from VerticalEdgeCreator
		[[nodiscard]] std::shared_ptr<Edge> createCrossDeckEdge(std::shared_ptr<VerticalEdgeCreator> edgeCreator) const override;
	};

} // core

#pragma once
