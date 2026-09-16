#pragma once

#include "core/Vertex.h"


namespace core
{
	class StairwellLocationVertex : public Vertex
	{
	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		StairwellLocationVertex(uint32_t id, std::shared_ptr<Sector> sector, float xLocationOffset, float yLocationOffset);

		StairwellLocationVertex(std::shared_ptr<Sector> sector, float xLocationOffset, float yLocationOffset);

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
