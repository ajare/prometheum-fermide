#include "core/ShuttleOrchestratedSystem.h"
#include "core/Button.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	ShuttleOrchestratedSystem::ShuttleOrchestratedSystem(shared_ptr<Orchestrator> orchestrator)
		: OrchestratedSystem(orchestrator)
	{
	}

	void ShuttleOrchestratedSystem::onDoorOpened(Controllable const* door, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data)
	{
		addLogMessage("ShuttleOrchestratedSystem", ~0u, LogLevel::Debug, "ON: door opened");

		mShuttle->getShuttle()->onDoorOpened(static_cast<Door const*>(door));
	}

	void ShuttleOrchestratedSystem::onDoorClosed(Controllable const* door, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data)
	{
		addLogMessage("ShuttleOrchestratedSystem", ~0u, LogLevel::Debug, "ON: door closed");

		mShuttle->getShuttle()->onDoorClosed(static_cast<Door const*>(door));
	}

	void ShuttleOrchestratedSystem::onShuttleArrivedAtStop(Controllable const* shuttle, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data)
	{
		addLogMessage("ShuttleOrchestratedSystem", ~0u, LogLevel::Debug, "ON: shuttle arrived");

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
					bind(&ShuttleOrchestratedSystem::onDoorOpened,
						this,
						placeholders::_1,
						placeholders::_2,
						placeholders::_3,
						placeholders::_4
					)
				);
			}
		}
	}

	void ShuttleOrchestratedSystem::setShuttle(shared_ptr<ShuttleTransit> shuttle)
	{
		if (mShuttle)
		{
			throw Exception("Cannot set Shuttle as it already has one set.");
		}

		mShuttle = shuttle;
	}

	void ShuttleOrchestratedSystem::addStop(uint32_t stopIndex, vector<shared_ptr<Door>> const& doors, vector<shared_ptr<Button>> const& buttons)
	{
		if (!mShuttle)
		{
			throw Exception("You must set the Shuttle first, before adding Buttons, otherwise the Buttons cannot be added to the Shuttle as Controllers.");
		}

		assert(doors.size() == buttons.size());

		for (uint32_t i = 0; i < (uint32_t)doors.size(); ++i)
		{
			auto button = buttons[i];
			auto door = doors[i];
			auto buttonPtr = buttons[i].get();

			mStops[buttonPtr] = { door.get(), stopIndex };
			buttonPtr->setOrchestrator(mOrchestrator);

			door->_setTypeCallback(ControllableActionType::Close,
				bind(&ShuttleOrchestratedSystem::onDoorClosed,
					this,
					placeholders::_1,
					placeholders::_2,
					placeholders::_3,
					placeholders::_4
				));

			addController(buttonPtr);
			mShuttle->getShuttle()->_addController(button);
		}
	}

	ControllableActionStatus ShuttleOrchestratedSystem::handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data)
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
				actionId = mShuttle->getShuttle()->handleAction(
					ControllableActionType::CallToStop,
					false,
					data,
					bind(&ShuttleOrchestratedSystem::onShuttleArrivedAtStop,
						this,
						placeholders::_1,
						placeholders::_2,
						placeholders::_3,
						placeholders::_4
					)
				);

				status = actionId != ~0u ? mShuttle->getShuttle()->getActionStatus(actionId) : ControllableActionStatus::Unhandled;
				break;

			default:
				break;
			}
		}

		return status;
	}

} // core