#pragma once

#include <cstdint>
#include <memory>

#include "core/RailedTransport.h"
#include "core/EntityId.h"


namespace core
{

	class Shuttle : public RailedTransport
	{
		friend class Building;
		TraversalResourceId mTraversalResource;
		uint32_t mNumCars;

		uint32_t mCarWidth;

	public:

		Shuttle(uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, float transportWidth, float transportHeight, uint32_t numCars, uint32_t carWidth, std::vector<uint32_t> stopOffsets);

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		uint32_t getNumCars() const;

		uint32_t getCarWidth() const;

		[[nodiscard]] TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }
		void configureTraversal(TraversalResourceId resource) { mTraversalResource = resource; }
		void setCoordinatedPosition(float globalPosition)
		{
			auto position = Shape::getPosition();
			position.x = globalPosition;
			Shape::setPosition(position);
		}
	};

} // core
