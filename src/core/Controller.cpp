#include "core/Controller.h"
#include "core/Orchestrator.h"


namespace core
{
	using namespace std;

	Controller::Controller()
		: Useable()
		, mOrchestrator(nullptr)
	{
	}

	Controller::Controller(shared_ptr<Orchestrator> orchestrator)
		: mOrchestrator(orchestrator)
	{
	}

	void Controller::setOrchestrator(shared_ptr<Orchestrator> orchestrator)
	{
		mOrchestrator = orchestrator;
	}

	ControllableActionStatus Controller::sendActionToOrchestrator(ControllerActionType action, Controller* subject, ControllableActionData const& data)
	{
		if (mOrchestrator)
		{
			return mOrchestrator->handleAction(action, this, subject, data);
		}
		else
		{
			return ControllableActionStatus::Unhandled;
		}
	}

	bool Controller::canBeUsed(Controller const* controller) const
	{
		return isEnabled();
	}

} // core
