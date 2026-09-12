#pragma once

#include "core/Vertex.h"
#include "core/BulkheadDoor.h"


namespace core
{
	class Location;

	class BulkheadDoorVertex : public Vertex
	{
		std::shared_ptr<BulkheadDoor> mBulkheadDoor;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		BulkheadDoorVertex(uint32_t id, std::shared_ptr<Sector> sector, std::shared_ptr<BulkheadDoor> door, float xLocationOffset, float yLocationOffset);

		BulkheadDoorVertex(std::shared_ptr<Sector> sector, std::shared_ptr<BulkheadDoor> door, float xLocationOffset, float yLocationOffset);

		std::shared_ptr<BulkheadDoor> getBulkheadDoor() const;

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
