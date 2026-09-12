#include "core/DoorVertexController.h"
#include "core/DoorVertex.h"
#include "core/Location.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	DoorVertexController::DoorVertexController(SectorObject const* owner, Shape controlArea[2], Shape exitArea[2], Building const* building, shared_ptr<Vertex> vertices[2], uint32_t x, uint32_t y, uint32_t width)
		: VertexController(
			owner,
			make_shared<QueuedVertexControllerArea>(this, controlArea[0], exitArea[0], building, vertices[0], x, y, width),
			make_shared<QueuedVertexControllerArea>(this, controlArea[1], exitArea[1], building, vertices[1], x, y, width),
			VertexController::InterSectorExitType::X)
		, SensorSource()
		, mExitOffset(0.0f)
		, mHasControllers(false)
	{
	}

	DoorVertexController::DoorVertexController(SectorObject const* owner, Shape controlArea[2], Shape exitArea[2], Building const* building, shared_ptr<Vertex> vertices[2], uint32_t x, uint32_t y, uint32_t width, shared_ptr<Controller> controllers[2], shared_ptr<Vertex> controllerVertices[2])
		: VertexController(
			owner,
			make_shared<QueuedVertexControllerArea>(this, controlArea[0], exitArea[0], building, vertices[0], x, y, width, controllers[0], controllerVertices[0]),
			make_shared<QueuedVertexControllerArea>(this, controlArea[1], exitArea[1], building, vertices[1], x, y, width, controllers[1], controllerVertices[1]),
			VertexController::InterSectorExitType::X)
		, SensorSource()
		, mExitOffset(0.0f)
		, mHasControllers(true)
	{
	}

	string DoorVertexController::getDescription() const
	{
		return "DoorVertexController";
	}

	bool DoorVertexController::objectUseRequired() const
	{
		auto doorState = mDoor->getState();

		return doorState == OpenableObject::State::Closing || doorState == OpenableObject::State::Closed;
	}

	bool DoorVertexController::controllerUseRequired() const
	{
		auto doorState = mDoor->getState();

		return mHasControllers && (doorState == OpenableObject::State::Closing || doorState == OpenableObject::State::Closed);
	}

	bool DoorVertexController::canExit(Agent* agent) const
	{
		return mDoor->isOpen();
	}

	vector<shared_ptr<VertexControllerArea>> DoorVertexController::getAreasForLayer(uint32_t layerIndex) const
	{
		return {
			mAreas[layerIndex]
		};
	}

	shared_ptr<Vertex> DoorVertexController::getVertexForLayer(uint32_t layerIndex) const
	{
		auto area = getArea(layerIndex);

		return area->getVertex();
	}

	void DoorVertexController::setVertexForLayer(int layer, shared_ptr<Vertex> vertex)
	{
		ASSERT_LAYER_OK(layer);

		auto area = getArea(layer);
		area->setVertex(vertex);

		mDoor = static_pointer_cast<Door>(vertex->getObject());
	}

	void DoorVertexController::handleVertexAction(Agent* agent, shared_ptr<Vertex> vertex, VertexActionType type)
	{
		auto doorVertex = static_pointer_cast<DoorVertex>(vertex);
			
		switch (type)
		{
		case VertexActionType::UnblockForExit:
			// Callback for Door, while it is opening
			doorVertex->getDoor()->handleAction(ControllableActionType::Open, true, {}, bind(
				&DoorVertexController::onDoorOpened,
				this,
				placeholders::_1,
				placeholders::_2,
				placeholders::_3,
				placeholders::_4)
			);
			break;

		default:
			break;
		}
	}

	bool DoorVertexController::canSense(SensorType type) const
	{
		switch (type)
		{
		case SensorType::AgentBlocking:
			for (auto const& area : mAreas)
			{
				if (area->hasAgentReadyToExit())
				{
					return true;
				}
			}

			return false;
		
		default:
			throw UnhandledException(type, "SensorType");
		}
	}

	void DoorVertexController::onDoorOpened(Controllable const* door, ControllableActionType type, ControllableActionStatus status, ControllableActionData const& data)
	{
	}

	void DoorVertexController::updateImpl(float frameTime)
	{
	}

} // core
