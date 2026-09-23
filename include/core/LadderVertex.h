#pragma once

#include "core/Vertex.h"
#include "core/Ladder.h"


namespace core
{
	class Location;

	class LadderVertex : public Vertex
	{
		std::shared_ptr<Ladder> mLadder;

		int mEndpoint;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		LadderVertex(uint32_t id, std::shared_ptr<Sector> sector, std::shared_ptr<Ladder> ladder, float xOffset, float yOffset, int endpoint);

		LadderVertex(std::shared_ptr<Sector> sector, std::shared_ptr<Ladder> ladder, float xOffset, float yOffset, int endpoint);

		std::shared_ptr<Ladder> getLadder() const;

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
