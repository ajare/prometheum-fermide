#include "core/ButtonExtensibleObjectOrchestratedSystem.h"
#include "core/Button.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	ButtonExtensibleObjectOrchestratedSystem::ButtonExtensibleObjectOrchestratedSystem(shared_ptr<Orchestrator> orchestrator)
		: OrchestratedSystem(orchestrator)
	{
	}

	void ButtonExtensibleObjectOrchestratedSystem::setExtensibleObject(shared_ptr<ExtensibleObject> ExtensibleObject)
	{
		if (mExtensibleObject)
		{
			throw Exception("Cannot set ExtensibleObject as it already has one set.");
		}

		mExtensibleObject = ExtensibleObject;
	}

	void ButtonExtensibleObjectOrchestratedSystem::addButton(shared_ptr<Button> button)
	{
		if (!mExtensibleObject)
		{
			throw Exception("You must set the ExtensibleObject first, before adding Buttons, otherwise the Buttons cannot be added to the ExtensibleObject as Controllers.");
		}

		auto buttonPtr = button.get();

		auto it = find(mButtons.begin(), mButtons.end(), buttonPtr);

		if (it == mButtons.end())
		{
			mButtons.push_back(buttonPtr);
			addController(buttonPtr);

			// Set up Button
			buttonPtr->setOrchestrator(mOrchestrator);

			// Set up ExtensibleObject
			mExtensibleObject->_addController(button);
		}
	}

	void ButtonExtensibleObjectOrchestratedSystem::onExtensibleObjectToggled(Controllable const* ExtensibleObject, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data)
	{
		for (auto button : mButtons)
		{
			button->Useable::enable();
		}
	}

	ControllableActionStatus ButtonExtensibleObjectOrchestratedSystem::handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data)
	{
		uint32_t actionId;
		ControllableActionStatus status = ControllableActionStatus::Unhandled;

		auto it = find(mButtons.begin(), mButtons.end(), static_cast<Button*>(object));

		if (it != mButtons.end())
		{
			switch (action)
			{
			case ControllerActionType::Press:
				actionId = mExtensibleObject->handleAction(ControllableActionType::Toggle,
					true,
					{},
					bind(&ButtonExtensibleObjectOrchestratedSystem::onExtensibleObjectToggled,
						this,
						placeholders::_1,
						placeholders::_2,
						placeholders::_3,
						placeholders::_4
					)
				);

				status = actionId != ~0u ? mExtensibleObject->getActionStatus(actionId) : ControllableActionStatus::Unhandled;

				if (status == ControllableActionStatus::InProgress)
				{
					for (auto button : mButtons)
					{
						button->Useable::disable();
					}
				}
				break;

			default:
				break;
			}
		}

		return status;
	}

} // core