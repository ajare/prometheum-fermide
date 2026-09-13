#pragma once

#include <cstdint>
#include <memory>

#include "core/RailedTransport.h"
#include "core/EntityId.h"


namespace core
{

	class Lift : public RailedTransport
	{
		friend class Building;
		TraversalResourceId mTraversalResource;

	public:

		Lift(uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, float transportWidth, float transportHeight, float speed, std::vector<uint32_t> stopOffsets);

		[[nodiscard]] TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }
		void configureTraversal(TraversalResourceId resource) { mTraversalResource = resource; }
		// The replacement coordinator is authoritative for motion; keep the legacy
		// renderable shape synchronized without entering its callback state machine.
		void setCoordinatedPosition(float globalPosition)
		{
			auto position = Shape::getPosition();
			position.y = globalPosition;
			Shape::setPosition(position);
		}
	
	};

} // core
