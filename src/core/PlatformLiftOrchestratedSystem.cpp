#include "core/PlatformLiftOrchestratedSystem.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	PlatformLiftOrchestratedSystem::PlatformLiftOrchestratedSystem(shared_ptr<Orchestrator> orchestrator)
		: OrchestratedSystem(orchestrator)
	{
	}

	void PlatformLiftOrchestratedSystem::onLiftArrivedAtDeck(Controllable const* lift, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data)
	{
		addLogMessage("PlatformLiftOrchestratedSystem", ~0u, LogLevel::Debug, "ON: lift arrived");
	}

	void PlatformLiftOrchestratedSystem::setLift(shared_ptr<Lift> lift)
	{
		if (mLift)
		{
			throw Exception("Cannot set Lift as it already has one set.");
		}

		mLift = lift;
	}

	void PlatformLiftOrchestratedSystem::addStop(shared_ptr<Button> button)
	{
		if (!mLift)
		{
			throw Exception("You must set the Lift first, before adding Buttons, otherwise the Buttons cannot be added to the Lift as Controllers.");
		}

		auto buttonPtr = button.get();

		auto stopIndex = (uint32_t)mStops.size();
		mStops[buttonPtr] = {stopIndex };

		buttonPtr->setOrchestrator(mOrchestrator);

		addController(buttonPtr);
		mLift->_addController(button);
	}

	ControllableActionStatus PlatformLiftOrchestratedSystem::handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data)
	{
		uint32_t actionId;
		ControllableActionStatus status = ControllableActionStatus::Unhandled;

		auto it = mStops.find(static_cast<Button*>(object));

		if (it != mStops.end())
		{
			auto stopIndex = it->second.index;

			ControllableActionData data;
			data.i = stopIndex;

			switch (action)
			{
			case ControllerActionType::Press:
				actionId = mLift->handleAction(
					ControllableActionType::CallToStop,
					false,
					data,
					bind(&PlatformLiftOrchestratedSystem::onLiftArrivedAtDeck,
						this,
						placeholders::_1,
						placeholders::_2,
						placeholders::_3,
						placeholders::_4
					)
				);

				status = actionId != ~0u ? mLift->getActionStatus(actionId) : ControllableActionStatus::Unhandled;
				break;

			default:
				break;
			}
		}

		return status;
	}

} // core