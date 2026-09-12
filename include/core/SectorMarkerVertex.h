#pragma once

#include "core/Vertex.h"


namespace core
{
	class SectorMarkerVertex : public Vertex
	{
	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		SectorMarkerVertex(uint32_t id, std::shared_ptr<Sector> sector, float xLocationOffset, float yLocationOffset);

		SectorMarkerVertex(std::shared_ptr<Sector> sector, float xLocationOffset, float yLocationOffset);

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
