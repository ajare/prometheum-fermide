#pragma once

#include "core/Vertex.h"
#include "core/Staircase.h"


namespace core
{
	class Location;

	class StaircaseVertex : public Vertex
	{
		std::shared_ptr<Staircase> mStaircase;

		uint32_t mDeckOffset;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		StaircaseVertex(uint32_t id, std::shared_ptr<Sector> sector, std::shared_ptr<Staircase> staircase, float xOffset, float yOffset, uint32_t deckOffset);

		StaircaseVertex(std::shared_ptr<Sector> sector, std::shared_ptr<Staircase> staircase, float xOffset, float yOffset, uint32_t deckOffset);

		std::shared_ptr<Staircase> getStaircase() const;

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
