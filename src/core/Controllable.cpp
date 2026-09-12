#include "core/Controllable.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	uint32_t Controllable::ActionIdGenerator = 0;

	uint32_t Controllable::IdGenerator = 1;

	Controllable::Controllable()
		: Useable()
		, mControllableId(IdGenerator++)
	{
	}

	uint32_t Controllable::getId() const
	{
		return mControllableId;
	}

	uint32_t Controllable::getNumControllers() const
	{
		return (uint32_t)mControllers.size();
	}

	void Controllable::_addController(shared_ptr<Controller> controller)
	{
		mControllers.push_back(controller);
	}

	shared_ptr<Controller> Controllable::getController(uint32_t index) const
	{
		return mControllers[index];
	}

	void Controllable::_setTypeCallback(ControllableActionType type, ControllableActionCallback callback)
	{
		mTypeCallbacks[(int)type] = callback;
	}

	ControllableAction const& Controllable::getAction(uint32_t id) const
	{
		for (auto& action : mActions)
		{
			if (action.id == id)
			{
				return action;
			}
		}

		throw Exception(format("Could not get ControllableAction with id: {}", id));
	}

	ControllableAction& Controllable::getAction(uint32_t id)
	{
		for (auto& action : mActions)
		{
			if (action.id == id)
			{
				return action;
			}
		}

		throw Exception(format("Could not get ControllableAction with id: {}", id));
	}

	ControllableAction& Controllable::getCurrentAction()
	{
		return mActions.back();
	}

	ControllableAction const& Controllable::getCurrentAction() const
	{
		return mActions.back();
	}

	ControllableActionStatus Controllable::getActionStatus(uint32_t id) const
	{
		auto const& action = getAction(id);
		
		return action.status;
	}

	void Controllable::popCurrentAction()
	{
		auto const& action = mActions.back();

		addLogMessage(getDescription(), getId(), LogLevel::Debug, format("Remove Action {}", action.toString()));

		mActions.pop_back();
	}

	void Controllable::sortActionsByPriority(vector<ControllableAction>& actions)
	{
	}

	void Controllable::insertNewAction(ControllableAction const& action)
	{
		addLogMessage(getDescription(), getId(), LogLevel::Debug, format("Insert Action {}", action.toString()));

		mActions.push_back(action);
		
		sortActionsByPriority(mActions);
	}

	void Controllable::finaliseCurrentAction()
	{
		auto& action = getCurrentAction();

		addLogMessage(getDescription(), getId(), LogLevel::Debug, format("Finalise Action {}", action.toString()));

		if (action.callback)
		{
			action.callback(this, action.type, action.status, action.data);
		}

		// Generic callback
		auto typeAction = mTypeCallbacks[(int)action.type];

		if (typeAction)
		{
			typeAction(this, action.type, action.status, action.data);
		}

		popCurrentAction();
	}

	void Controllable::processCurrentAction(float frameTime)
	{
		auto& action = getCurrentAction();

		action.status = updateAction(action, frameTime);

		if (action.finished())
		{
			addLogMessage(getDescription(), getId(), LogLevel::Debug, format("Finish Action {}", action.toString()));
			finishAction(action);
			finaliseCurrentAction();

			// Start new action
			if (!mActions.empty())
			{
				auto& nextAction = getCurrentAction();

				if (nextAction.autoTrigger)
				{
					addLogMessage(getDescription(), getId(), LogLevel::Debug, "Auto trigger next action");
					addLogMessage(getDescription(), getId(), LogLevel::Debug, format("Start Action {}", nextAction.toString()));
					nextAction.status = startAction(nextAction);
				}
			}
		}
	}

	bool Controllable::interruptAction(ControllableActionType newType, ControllableActionType curType)
	{
		return newType != curType;
	}

	bool Controllable::modifyAndReject(ControllableActionType type, ControllableActionData const& data)
	{
		return false;
	}

	bool Controllable::validateAction(ControllableActionType type) const
	{
		return false;
	}

	uint32_t Controllable::handleAction(ControllableActionType type, bool autoTrigger, ControllableActionData data, ControllableActionCallback callback)
	{
		if (!validateAction(type))
		{
			throw Exception(format("Controllable {} does not support the action '{}'", getDescription(), getControllableActionTypeString(type)));
		}

		if (modifyAndReject(type, data))
		{
			return ~0u;
		}

		bool startThisAction{ true };

		// Check no action / callback in progress
		if (!mActions.empty())
		{
			auto& curAction = getCurrentAction();

			if (interruptAction(type, curAction.type))
			{
				curAction.status = ControllableActionStatus::CompletedInterrupted;
				
				finaliseCurrentAction();
			}
			else
			{
				startThisAction = false;
			}
		}

		// Add new action
		ControllableAction newAction;

		newAction.type = type;
		newAction.callback = callback;
		newAction.id = ActionIdGenerator++;
		newAction.data = data;
		newAction.status = ControllableActionStatus::Pending;
		newAction.autoTrigger = autoTrigger;

		insertNewAction(newAction);

		if (startThisAction)
		{
			auto& curAction = getCurrentAction();

			assert(curAction.id == newAction.id);

			addLogMessage(getDescription(), getId(), LogLevel::Debug, format("Start Action {}", curAction.toString()));
			curAction.status = startAction(curAction);

			// Some actions complete immediately.
			switch (curAction.status)
			{
			case ControllableActionStatus::CompletedSuccess:
			case ControllableActionStatus::CompletedFailure:
				finaliseCurrentAction();
				break;
			}
		}

		return newAction.id;
	}

	ControllableActionStatus Controllable::startAction(ControllableAction const& action)
	{
		return ControllableActionStatus::Unhandled;
	}

	void Controllable::finishAction(ControllableAction const& action)
	{
	}

	ControllableActionStatus Controllable::updateAction(ControllableAction const& action, float frameTime)
	{
		return ControllableActionStatus::Unhandled;
	}

	void Controllable::updateImpl(float frameTime, ControllableActionType action, ControllableActionStatus status)
	{
	}

	bool Controllable::canBeUsed(Controller const* controller) const
	{
		// A Controllable can be used if it has no Controllers
		return mControllers.empty() && isEnabled();
	}

	void Controllable::update(float frameTime)
	{
		ControllableActionType actionType = ControllableActionType::None;
		ControllableActionStatus actionStatus = ControllableActionStatus::None;
		uint32_t actionId = 0;

		if (!mActions.empty())
		{
			auto& action = getCurrentAction();

			actionId = action.id;
			actionType = action.type;

			processCurrentAction(frameTime);
		}

		updateImpl(frameTime, actionType, actionStatus);
	}

} // core
