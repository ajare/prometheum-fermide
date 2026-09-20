#pragma once

#include "core/Vertex.h"
#include "core/Door.h"


namespace core
{
	class Location;

	class DoorVertex : public Vertex
	{
		std::shared_ptr<Door> mDoor;
		float mCrossingWidth;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		DoorVertex(uint32_t id, std::shared_ptr<Sector> sector, std::shared_ptr<Door> door, float xLocationOffset, float yLocationOffset);

		DoorVertex(std::shared_ptr<Sector> sector, std::shared_ptr<Door> door, float xLocationOffset, float yLocationOffset);

		std::shared_ptr<Door> getDoor() const;

		// Symmetric distance either side of this vertex's x position within which
		// an agent on the threshold row may begin crossing (ticket #97). Derived
		// from the Door's physical doorway width minus the agent's width; never
		// serialized.
		float getCrossingWidth() const { return mCrossingWidth; }

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
