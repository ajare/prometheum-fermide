#include "core/LightingOrchestratedSystem.h"
#include "core/Button.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	LightingOrchestratedSystem::LightingOrchestratedSystem(shared_ptr<Orchestrator> orchestrator)
		: OrchestratedSystem(orchestrator)
	{
	}

	void LightingOrchestratedSystem::setSector(shared_ptr<Sector> Sector)
	{
		if (mSector)
		{
			throw Exception("Cannot set Sector as it already has one set.");
		}

		mSector = Sector;
	}

	void LightingOrchestratedSystem::addButton(shared_ptr<Button> button)
	{
		if (!mSector)
		{
			throw Exception("You must set the Sector first, before adding Buttons, otherwise the Buttons cannot be added to the Sector as Controllers.");
		}

		auto buttonPtr = button.get();

		auto it = find(mButtons.begin(), mButtons.end(), buttonPtr);

		if (it == mButtons.end())
		{
			mButtons.push_back(buttonPtr);
			addController(buttonPtr);

			// Set up Button
			buttonPtr->setOrchestrator(mOrchestrator);

			// Set up Sector
			mSector->_addController(button);
		}
	}

	void LightingOrchestratedSystem::onLightsToggled(Controllable const* Lighting, ControllableActionType action, ControllableActionStatus status, ControllableActionData data)
	{
	}

	ControllableActionStatus LightingOrchestratedSystem::handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data)
	{
		uint32_t actionId;
		ControllableActionStatus status = ControllableActionStatus::Unhandled;

		auto it = find(mButtons.begin(), mButtons.end(), static_cast<Button*>(object));

		if (it != mButtons.end())
		{
			switch (action)
			{
			case ControllerActionType::Press:
				actionId = mSector->handleAction(ControllableActionType::ToggleLights);
				status = mSector->getActionStatus(actionId);
				break;

			default:
				break;
			}
		}

		return status;
	}

} // core