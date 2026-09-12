#include "core/BulkheadDoorVertexController.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	BulkheadDoorVertexController::BulkheadDoorVertexController(SectorObject const* owner, Shape controlArea[2])
		: VertexController(
			owner,
			make_shared<VertexControllerArea>(this, controlArea[0]),
			make_shared<VertexControllerArea>(this, controlArea[1]),
			VertexController::InterSectorExitType::X)
		, mVertices{ nullptr, nullptr }
	{
	}

	string BulkheadDoorVertexController::getDescription() const
	{
		return "BulkheadDoorVertexController";
	}

	vector<shared_ptr<VertexControllerArea>> BulkheadDoorVertexController::getAreasForLayer(uint32_t layerIndex) const
	{
		return {
			mAreas[0],
			mAreas[1]
		};
	}

	bool BulkheadDoorVertexController::inControlArea(SectorPosition const& pos, uint32_t* index) const
	{
		for (int i = 0; i < MaxAreaCount; ++i)
		{
			if (mAreas[i]->inControlArea(pos.global()))
			{
				*index = (uint32_t)i;
				return true;
			}
		}

		*index = ~0u;
		return false;
	}

	bool BulkheadDoorVertexController::objectUseRequired() const
	{
		return false;
	}

	bool BulkheadDoorVertexController::canExit(Agent* agent) const
	{
		return false;
	}

	void BulkheadDoorVertexController::setVertexForSide(int side, shared_ptr<Vertex> vertex)
	{
		ASSERT_SIDE_OK(side);

		auto area = getArea(side);
		area->setVertex(vertex);

		mVertices[side] = vertex;
	}

	void BulkheadDoorVertexController::handleVertexAction(Agent* agent, shared_ptr<Vertex> vertex, VertexActionType type)
	{
	}

} // core
