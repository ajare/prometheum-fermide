#pragma once

#include <memory>
#include <vector>

#include "core/OrchestratedSystem.h"
#include "core/Button.h"
#include "core/BulkheadDoor.h"


namespace core
{
	class Orchestrator;

	class BulkheadDoorOrchestratedSystem : public OrchestratedSystem
	{
		std::shared_ptr<BulkheadDoor> mDoor;

		std::vector<Button*> mButtons;

	private:

		void onBulkheadDoorOpenedAndClosed(Controllable const* bulkheadDoor, ControllableActionType action, ControllableActionStatus status, ControllableActionData data);

	public:

		explicit BulkheadDoorOrchestratedSystem(std::shared_ptr<Orchestrator> orchestrator);

		~BulkheadDoorOrchestratedSystem() = default;

		void setBulkheadDoor(std::shared_ptr<BulkheadDoor> door);

		void addButton(std::shared_ptr<Button> button);

		ControllableActionStatus handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data) override;
	};

} // core
