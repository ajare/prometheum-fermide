#include <cassert>

#include "core/OrchestratedSystem.h"


namespace core
{

	using namespace std;

	OrchestratedSystem::OrchestratedSystem(shared_ptr<Orchestrator> orchestrator)
		: mOrchestrator(orchestrator)
	{
	}

	vector<Controller*> OrchestratedSystem::getControllers() const
	{
		return mControllers;
	}

	void OrchestratedSystem::addController(Controller* controller)
	{
		mControllers.push_back(controller);
	}

} // core