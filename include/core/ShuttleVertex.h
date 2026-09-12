#pragma once

#include "core/ControllerVertex.h"
#include "core/Shuttle.h"


namespace core
{
	class Location;

	class ShuttleVertex : public Vertex
	{
		std::shared_ptr<Shuttle> mShuttle;

		uint32_t mStopOffset;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		ShuttleVertex(uint32_t id, std::shared_ptr<Sector> sector, std::shared_ptr<Shuttle> shuttle, float xOffset, float yOffset, uint32_t stopOffset);

		ShuttleVertex(std::shared_ptr<Sector> sector, std::shared_ptr<Shuttle> Shuttle, float xOffset, float yOffset, uint32_t stopOffset);

		std::shared_ptr<Shuttle> getShuttle() const;

		std::shared_ptr<Vertex> copyWithoutEdges() override;

		std::string getDescription() const override;
	};

} // core
