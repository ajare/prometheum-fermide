#pragma once

#include "core/Vertex.h"
#include "core/Door.h"


namespace core
{
	class Location;

	class DoorVertex : public Vertex
	{
		std::shared_ptr<Door> mDoor;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		DoorVertex(uint32_t id, std::shared_ptr<Sector> sector, std::shared_ptr<Door> door, float xLocationOffset, float yLocationOffset);

		DoorVertex(std::shared_ptr<Sector> sector, std::shared_ptr<Door> door, float xLocationOffset, float yLocationOffset);

		std::shared_ptr<Door> getDoor() const;

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
