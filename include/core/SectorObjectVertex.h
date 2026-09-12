#pragma once

#include "core/Vertex.h"


namespace core
{
	class SectorObject;

	class SectorObjectVertex : public Vertex
	{
		std::shared_ptr<SectorObject> mObject;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		SectorObjectVertex(uint32_t id, VertexSubType subType, std::shared_ptr<Sector> sector, std::shared_ptr<SectorObject> object, float xLocationOffset, float yLocationOffset);

		SectorObjectVertex(VertexSubType subType, std::shared_ptr<Sector> sector, std::shared_ptr<SectorObject> object, float xLocationOffset, float yLocationOffset);

		std::shared_ptr<SectorObject> getObject() const;

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
