#include "core/Orchestrator.h"


namespace core
{

	using namespace std;

	Orchestrator::Orchestrator()
	{
	}

	void Orchestrator::addSystem(shared_ptr<OrchestratedSystem> system)
	{
		auto controllers = system->getControllers();

		for (auto controller : controllers)
		{
			mControllerMap.insert(make_pair(controller, system));
		}
	}

	ControllableActionStatus Orchestrator::handleAction(ControllerActionType action, Controller* object, Controller* subject, ControllableActionData const& data)
	{
		auto res = ControllableActionStatus::None;

		for (auto [it, rangeEnd] = mControllerMap.equal_range(object); it != rangeEnd; ++it)
		{
			auto controller = it->first;
			auto system = it->second;

			auto res0 = system->handleAction(object, subject, action, data);

			// We have to pack multiple results into one: return highest severity
			if ((int)res0 > (int)res)
			{
				res = res0;
			}
		}

		if (res == ControllableActionStatus::None)
		{
			res = ControllableActionStatus::Unhandled;
		}
		
		return res;
	}

} // core
