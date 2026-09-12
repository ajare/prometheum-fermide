#include "core/ButtonDoorOrchestratedSystem.h"
#include "core/Button.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	ButtonDoorOrchestratedSystem::ButtonDoorOrchestratedSystem(shared_ptr<Orchestrator> orchestrator)
		: OrchestratedSystem(orchestrator)
	{
	}

	void ButtonDoorOrchestratedSystem::setDoor(shared_ptr<Door> door)
	{
		if (mDoor)
		{
			throw Exception("Cannot set Door as it already has one set.");
		}

		mDoor = door;
	}

	void ButtonDoorOrchestratedSystem::addButton(shared_ptr<Button> button)
	{
		if (!mDoor)
		{
			throw Exception("You must set the Door first, before adding Buttons, otherwise the Buttons cannot be added to the Door as Controllers.");
		}

		auto buttonPtr = button.get();

		auto it = find(mButtons.begin(), mButtons.end(), buttonPtr);

		if (it == mButtons.end())
		{
			mButtons.push_back(buttonPtr);
			addController(buttonPtr);

			// Set up Button
			buttonPtr->setOrchestrator(mOrchestrator);

			// Set up Door
			mDoor->_addController(button);
		}
	}

	void ButtonDoorOrchestratedSystem::onDoorOpened(Controllable const* door, ControllableActionType action, ControllableActionStatus status, ControllableActionData data)
	{
	}

	ControllableActionStatus ButtonDoorOrchestratedSystem::handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data)
	{
		uint32_t actionId;
		ControllableActionStatus status = ControllableActionStatus::Unhandled;

		auto it = find(mButtons.begin(), mButtons.end(), static_cast<Button*>(object));

		if (it != mButtons.end())
		{
			switch (action)
			{
			case ControllerActionType::Press:
				actionId = mDoor->handleAction(ControllableActionType::Open, 
					true,
					{},
					bind(&ButtonDoorOrchestratedSystem::onDoorOpened, 
						this, 
						placeholders::_1, 
						placeholders::_2, 
						placeholders::_3,
						placeholders::_4
				));

				status = actionId != ~0u ? mDoor->getActionStatus(actionId) : ControllableActionStatus::Unhandled;
				break;

			case ControllerActionType::Toggle:
				actionId = mDoor->handleAction(ControllableActionType::Toggle);
				status = actionId != ~0u ? mDoor->getActionStatus(actionId) : ControllableActionStatus::Unhandled;
				break;

			default:
				break;
			}
		}

		return status;
	}

} // core