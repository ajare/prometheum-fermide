#include "core/BulkheadDoorOrchestratedSystem.h"
#include "core/Button.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	BulkheadDoorOrchestratedSystem::BulkheadDoorOrchestratedSystem(shared_ptr<Orchestrator> orchestrator)
		: OrchestratedSystem(orchestrator)
	{
	}

	void BulkheadDoorOrchestratedSystem::setBulkheadDoor(shared_ptr<BulkheadDoor> door)
	{
		if (mDoor)
		{
			throw Exception("Cannot set BulkheadDoor as it already has one set.");
		}

		mDoor = door;
		
		mDoor->_setTypeCallback(ControllableActionType::Close, 
			bind(&BulkheadDoorOrchestratedSystem::onBulkheadDoorOpenedAndClosed,
				this,
				placeholders::_1,
				placeholders::_2,
				placeholders::_3,
				placeholders::_4
		));
	}

	void BulkheadDoorOrchestratedSystem::addButton(shared_ptr<Button> button)
	{
		if (!mDoor)
		{
			throw Exception("You must set the BulkheadDoor first, before adding Buttons, otherwise the Buttons cannot be added to the BulkheadDoor as Controllers.");
		}

		auto buttonPtr = button.get();

		auto it = find(mButtons.begin(), mButtons.end(), buttonPtr);

		if (it == mButtons.end())
		{
			mButtons.push_back(buttonPtr);
			addController(buttonPtr);

			// Set up Button
			buttonPtr->setOrchestrator(mOrchestrator);

			// Set up ForceBridge
			mDoor->_addController(button);
		}
	}

	void BulkheadDoorOrchestratedSystem::onBulkheadDoorOpenedAndClosed(Controllable const* bulkheadDoor, ControllableActionType action, ControllableActionStatus status, ControllableActionData data)
	{
		for (auto button : mButtons)
		{
			button->Useable::enable();
		}
	}

	ControllableActionStatus BulkheadDoorOrchestratedSystem::handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data)
	{
		uint32_t actionId;
		ControllableActionStatus status = ControllableActionStatus::Unhandled;

		auto it = find(mButtons.begin(), mButtons.end(), static_cast<Button*>(object));

		if (it != mButtons.end())
		{
			switch (action)
			{
			case ControllerActionType::Press:
				actionId = mDoor->handleAction(ControllableActionType::Open);
				status = actionId != ~0u ? mDoor->getActionStatus(actionId) : ControllableActionStatus::Unhandled;

				if (status == ControllableActionStatus::InProgress)
				{
					for (auto button : mButtons)
					{
						button->Useable::disable();
					}
				}
				break;

			default:
				status = ControllableActionStatus::Unhandled;
				break;
			}
		}

		return status;
	}

} // core