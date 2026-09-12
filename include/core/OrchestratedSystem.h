#pragma once

#include <memory>
#include <map>
#include <vector>

#include "core/Controller.h"
#include "core/ControllerActionType.h"
#include "core/ControllableActionStatus.h"
#include "core/ControllableActionData.h"


namespace core
{
	class Orchestrator;

	class OrchestratedSystem
	{
		std::vector<Controller*> mControllers;

	protected:

		std::shared_ptr<Orchestrator> mOrchestrator;

	protected:

		void addController(Controller* controller);

	public:

		OrchestratedSystem(std::shared_ptr<Orchestrator> orchestrator);

		virtual ~OrchestratedSystem() = default;
	
		[[nodiscard]] std::vector<Controller*> getControllers() const;

		virtual ControllableActionStatus handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data) = 0;
	};

} // core
