#include "core/LiftOrchestratedSystem.h"
#include "core/Button.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	LiftOrchestratedSystem::LiftOrchestratedSystem(shared_ptr<Orchestrator> orchestrator)
		: OrchestratedSystem(orchestrator)
	{
	}

	void LiftOrchestratedSystem::onDoorOpened(Controllable const* door, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data)
	{
		addLogMessage("LiftOrchestratedSystem", ~0u, LogLevel::Debug, "ON: door opened");

		mLift->getLift()->onDoorOpened(static_cast<Door const*>(door));
	}

	void LiftOrchestratedSystem::onDoorClosed(Controllable const* door, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data)
	{
		addLogMessage("LiftOrchestratedSystem", ~0u, LogLevel::Debug, "ON: door closed");
		
		mLift->getLift()->onDoorClosed(static_cast<Door const*>(door));
	}

	void LiftOrchestratedSystem::onLiftArrivedAtDeck(Controllable const* lift, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data)
	{
		addLogMessage("LiftOrchestratedSystem", ~0u, LogLevel::Debug, "ON: lift arrived");

		// Open doors
		for (auto& kvp : mStops)
		{
			auto& [button, stop] = kvp;

			if (stop.index == (uint32_t)data.i)
			{
				stop.door->handleAction(
					ControllableActionType::Open,
					true,
					{},
					bind(&LiftOrchestratedSystem::onDoorOpened,
						this,
						placeholders::_1,
						placeholders::_2,
						placeholders::_3,
						placeholders::_4
					)
				);

				return;
			}
		}
	}

	void LiftOrchestratedSystem::setLift(shared_ptr<LiftTransit> lift)
	{
		if (mLift)
		{
			throw Exception("Cannot set Lift as it already has one set.");
		}

		mLift = lift;
	}

	void LiftOrchestratedSystem::addStop(shared_ptr<Door> door, shared_ptr<Button> button)
	{
		if (!mLift)
		{
			throw Exception("You must set the Lift first, before adding Buttons, otherwise the Buttons cannot be added to the Lift as Controllers.");
		}

		auto buttonPtr = button.get();

		auto stopIndex = (uint32_t)mStops.size();
		mStops[buttonPtr] = { door.get(), stopIndex };

		buttonPtr->setOrchestrator(mOrchestrator);

		door->_setTypeCallback(ControllableActionType::Close,
			bind(&LiftOrchestratedSystem::onDoorClosed,
				this,
				placeholders::_1,
				placeholders::_2,
				placeholders::_3,
				placeholders::_4
			));

		addController(buttonPtr);
		mLift->getLift()->_addController(button);
	}

	ControllableActionStatus LiftOrchestratedSystem::handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data)
	{
		uint32_t actionId;
		ControllableActionStatus status = ControllableActionStatus::Unhandled;

		auto it = mStops.find(static_cast<Button*>(object));

		if (it != mStops.end())
		{
			auto& [door, stopIndex] = it->second;
			
			ControllableActionData data;
			data.i = stopIndex;

			switch (action)
			{
			case ControllerActionType::Press:
				actionId = mLift->getLift()->handleAction(
					ControllableActionType::CallToStop, 
					false,
					data,
					bind(&LiftOrchestratedSystem::onLiftArrivedAtDeck,
						this,
						placeholders::_1,
						placeholders::_2,
						placeholders::_3,
						placeholders::_4
					)
				);

				status = actionId != ~0u ? mLift->getLift()->getActionStatus(actionId) : ControllableActionStatus::Unhandled;
				break;

			default:
				break;
			}
		}

		return status;
	}

} // core