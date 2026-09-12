#pragma once

#include <memory>
#include <string>

#include "core/Useable.h"
#include "core/ControllerActionType.h"
#include "core/ControllableActionData.h"


namespace core
{
	class Orchestrator;

	// See Controllable: dual-role objects must have one shared Useable base.
	class Controller : public virtual Useable
	{
		std::shared_ptr<Orchestrator> mOrchestrator;

	protected:

		ControllableActionStatus sendActionToOrchestrator(ControllerActionType action, Controller* subject, ControllableActionData const& data = {});

	public:

		Controller();

		explicit Controller(std::shared_ptr<Orchestrator> orchestrator);

		virtual ~Controller() = default;

		void setOrchestrator(std::shared_ptr<Orchestrator> orchestrator);

		// Overridden from Useable
		bool canBeUsed(Controller const* controller) const override;
	};

} // core

