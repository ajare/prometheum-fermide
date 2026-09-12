#pragma once

#include "core/ControllerVertex.h"
#include "core/Lift.h"


namespace core
{
	class Location;

	class LiftVertex : public Vertex
	{
		std::shared_ptr<Lift> mLift;

		uint32_t mStopOffset;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		LiftVertex(uint32_t id, std::shared_ptr<Sector> sector, std::shared_ptr<Lift> Lift, float xOffset, float yOffset, uint32_t stopOffset);

		LiftVertex(std::shared_ptr<Sector> sector, std::shared_ptr<Lift> Lift, float xOffset, float yOffset, uint32_t stopOffset);

		std::shared_ptr<Lift> getLift() const;

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
