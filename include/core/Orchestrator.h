#pragma once

#include <vector>
#include <memory>
#include <map>

#include "Controller.h"
#include "ControllerActionType.h"
#include "ControllableActionStatus.h"
#include "OrchestratedSystem.h"


namespace core
{

	class Orchestrator
	{
		std::vector<std::shared_ptr<OrchestratedSystem>> mSystems;

		std::multimap<Controller*, std::shared_ptr<OrchestratedSystem>> mControllerMap;

	public:

		Orchestrator();

		virtual ~Orchestrator() = default;

		void addSystem(std::shared_ptr<OrchestratedSystem> system);

		virtual ControllableActionStatus handleAction(ControllerActionType action, Controller* object, Controller* subject, ControllableActionData const& data);
	};

} // core
