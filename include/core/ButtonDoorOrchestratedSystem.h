#pragma once

#include <memory>
#include <vector>

#include "core/OrchestratedSystem.h"
#include "core/Button.h"
#include "core/Door.h"


namespace core
{
	class Orchestrator;

	class ButtonDoorOrchestratedSystem : public OrchestratedSystem
	{
		std::shared_ptr<Door> mDoor;

		std::vector<Button*> mButtons;

	private:

		void onDoorOpened(Controllable const* door, ControllableActionType action, ControllableActionStatus status, ControllableActionData data);

	public:

		explicit ButtonDoorOrchestratedSystem(std::shared_ptr<Orchestrator> orchestrator);

		~ButtonDoorOrchestratedSystem() = default;

		void setDoor(std::shared_ptr<Door> door);

		void addButton(std::shared_ptr<Button> button);

		ControllableActionStatus handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data) override;
	};

} // core
