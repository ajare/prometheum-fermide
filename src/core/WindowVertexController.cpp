#include "core/WindowVertexController.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	WindowVertexController::WindowVertexController(SectorObject const* owner, Shape controlArea)
		: VertexController(
			owner,
			make_shared<VertexControllerArea>(this, controlArea),
			VertexController::InterSectorExitType::X)
		, mVertex(nullptr)
	{
	}

	string WindowVertexController::getDescription() const
	{
		return "WindowVertexController";
	}

	vector<shared_ptr<VertexControllerArea>> WindowVertexController::getAreasForLayer(uint32_t layerIndex) const
	{
		return {
			mAreas[0]
		};
	}

	bool WindowVertexController::objectUseRequired() const
	{
		return false;
	}

	bool WindowVertexController::canExit(Agent* agent) const
	{
		return false;
	}

	void WindowVertexController::setVertex(shared_ptr<Vertex> vertex)
	{
		auto area = getArea(0);
		area->setVertex(vertex);

		mVertex = vertex;
	}

	void WindowVertexController::handleVertexAction(Agent* agent, shared_ptr<Vertex> vertex, VertexActionType type)
	{
	}

} // core
