#pragma once

#include <vector>

#include "core/VertexControllerArea.h"
#include "core/Defines.h"


namespace core
{
	class Building;

	class QueuedVertexControllerArea : public VertexControllerArea
	{
		std::vector<float> calculateDoorQueueStopOffsets(Building const* building, std::shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t width) const;

	public:

		QueuedVertexControllerArea(VertexController* owner, Shape const& controlArea, Building const* building, std::shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t width);

		QueuedVertexControllerArea(VertexController* owner, Shape const& controlArea, Shape const& exitArea, Building const* building, std::shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t width);

		QueuedVertexControllerArea(VertexController* owner, Shape const& controlArea, Building const* building, std::shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t width, std::shared_ptr<Controller> controller, std::shared_ptr<Vertex> controllerVertex);

		QueuedVertexControllerArea(VertexController* owner, Shape const& controlArea, Shape const& exitArea, Building const* building, std::shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t width, std::shared_ptr<Controller> controller, std::shared_ptr<Vertex> controllerVertex);

		~QueuedVertexControllerArea() = default;

		void setSpotOffsets(std::vector<float> const& offsets);
	};

} // core

