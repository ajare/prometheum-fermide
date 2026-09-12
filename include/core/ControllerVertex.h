#pragma once

#include "core/Vertex.h"


namespace core
{
	class Controller;

	class ControllerVertex : public Vertex
	{
		std::shared_ptr<Controller> mController;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		ControllerVertex(uint32_t id, VertexType type, VertexSubType subType, std::shared_ptr<Sector> sector, std::shared_ptr<Controller> controller, float xLocationOffset, float yLocationOffset);

		ControllerVertex(VertexType type, VertexSubType subType, std::shared_ptr<Sector> sector, std::shared_ptr<Controller> controller, float xLocationOffset, float yLocationOffset);

		std::shared_ptr<Controller> getController() const;

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
