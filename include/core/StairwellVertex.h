#pragma once

#include "core/Vertex.h"
#include "core/Stairwell.h"


namespace core
{
	class Location;

	class StairwellVertex : public Vertex
	{
		std::shared_ptr<Stairwell> mStairwell;

		uint32_t mLevelOffset;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		StairwellVertex(uint32_t id, std::shared_ptr<Sector> sector, std::shared_ptr<Stairwell> stairwell, float xOffset, float yOffset, uint32_t levelOffset);

		StairwellVertex(std::shared_ptr<Sector> sector, std::shared_ptr<Stairwell> stairwell, float xOffset, float yOffset, uint32_t levelOffset);

		std::shared_ptr<Stairwell> getStairwell() const;

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
