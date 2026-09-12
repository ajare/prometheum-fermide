#pragma once

#include "core/Defines.h"
#include "core/VertexController.h"
#include "core/SensorSource.h"
#include "core/Door.h"
#include "core/QueuedVertexControllerArea.h"


namespace core
{
	class Agent;

	class DoorVertexController : public VertexController, public SensorSource
	{
		std::shared_ptr<Door> mDoor;

		float mExitOffset;

		bool mHasControllers;

	private:

		// Overridden from VertexController
		void updateImpl(float frameTime) override;

		void onDoorOpened(Controllable const* door, ControllableActionType type, ControllableActionStatus status, ControllableActionData const& data);

	public:

		DoorVertexController(SectorObject const* owner, Shape controlArea[2], Shape exitArea[2], Building const* building, std::shared_ptr<Vertex> vertices[2], uint32_t x, uint32_t y, uint32_t width);

		DoorVertexController(SectorObject const* owner, Shape controlArea[2], Shape exitArea[2], Building const* building, std::shared_ptr<Vertex> vertices[2], uint32_t x, uint32_t y, uint32_t width, std::shared_ptr<Controller> controllers[2], std::shared_ptr<Vertex> controllerVertices[2]);

		~DoorVertexController() = default;

		// Overridden from VertexController
		std::string getDescription() const override;

		// Overridden from VertexController
		bool objectUseRequired() const override;

		// Overridden from VertexController
		bool controllerUseRequired() const override;

		// Overridden from VertexController
		bool canExit(Agent* agent) const;

		// Overridden from VertexController
		std::vector<std::shared_ptr<VertexControllerArea>> getAreasForLayer(uint32_t layerIndex) const override;

		std::shared_ptr<Vertex> getVertexForLayer(uint32_t layerIndex) const;

		void setVertexForLayer(int layer, std::shared_ptr<Vertex> vertex);

		// Overridden from VertexController
		void handleVertexAction(Agent* agent, std::shared_ptr<Vertex> vertex, VertexActionType type) override;

		// Overridden from SensorSource
		bool canSense(SensorType type) const override;
	};

} // core

