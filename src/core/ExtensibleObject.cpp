#include <cassert>

#include "core/Defines.h"
#include "core/ExtensibleObject.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Extensible Object
	-----------------

	This is an intermediate class which implements logic for objects which can extend, for instance ForceBridges
	or Ladders.
	*/
	ExtensibleObject::ExtensibleObject(float x, float y, float width, float height, bool extensible, bool startExtended)
		: Object(x, y, width, height)
		, mIsExtensible(extensible)
		, mState(startExtended ? State::Extended : State::Retracted)
		, mExtendedPct(startExtended ? 1.0f : 0.0f)
	{
	}

	bool ExtensibleObject::isExtensible() const
	{
		return mIsExtensible;
	}

	ExtensibleObject::State const& ExtensibleObject::getState() const
	{
		return mState;
	}

	float ExtensibleObject::getExtendedPercentage() const
	{
		return mExtendedPct;
	}

	float ExtensibleObject::getMaxRetractedPercentage() const
	{
		return 0.0f;
	}

	bool ExtensibleObject::isExtended() const
	{
		return mState == State::Extended;
	}

	bool ExtensibleObject::isRetracted() const
	{
		return mState == State::Retracted;
	}

	bool ExtensibleObject::isExtending() const
	{
		return mState == State::Extending;
	}

	bool ExtensibleObject::isRetracting() const
	{
		return mState == State::Retracting;
	}

	bool ExtensibleObject::canBeUsed(Controller const* controller) const
	{
		return Controllable::canBeUsed(controller) && isExtensible();
	}

	bool ExtensibleObject::extend()
	{
		if (!isExtensible())
		{
			return false;
		}

		if (mState != State::Extended)
		{
			mState = State::Extending;
		}

		return true;
	}

	bool ExtensibleObject::retract()
	{
		if (!isExtensible())
		{
			return false;
		}

		if (mState != State::Retracted)
		{
			mState = State::Retracting;
		}

		return true;
	}

	bool ExtensibleObject::toggle()
	{
		if (!isExtensible())
		{
			return false;
		}

		switch (mState)
		{
		case State::Extended:
		case State::Extending:
			return retract();

		case State::Retracted:
		case State::Retracting:
			return extend();

		default:
			throw UnhandledException(mState, "ExtensibleObject::State");
		}
	}

	bool ExtensibleObject::validateAction(ControllableActionType type) const
	{
		return type == ControllableActionType::Toggle;
	}

	ControllableActionStatus ExtensibleObject::startAction(ControllableAction const& action)
	{
		switch (action.type)
		{
		case ControllableActionType::Toggle:
			toggle();
			return ControllableActionStatus::InProgress;

		default:
			throw UnhandledException(action.type, "ControllableActionType");
		}
	}

	void ExtensibleObject::finishAction(ControllableAction const& action)
	{
		switch (action.type)
		{
		case ControllableActionType::Toggle:
			if (mState == State::Extending)
			{
				mState = State::Extended;
				mExtendedPct = 1.0f;
			}
			else if (mState == State::Retracting)
			{
				mState = State::Retracted;
				mExtendedPct = getMaxRetractedPercentage();
			}
			break;

		default:
			throw UnhandledException(action.type, "ControllableActionType");
		}
	}

	ControllableActionStatus ExtensibleObject::updateAction(ControllableAction const& action, float frameTime)
	{
		switch (action.type)
		{
		case ControllableActionType::Toggle:
			if (mState == State::Extending)
			{
				mExtendedPct = min(mExtendedPct + frameTime / getExtendRetractTime(), 1.0f);
				return mExtendedPct >= 1.0f ? ControllableActionStatus::CompletedSuccess : ControllableActionStatus::InProgress;
			}
			else if (mState == State::Retracting)
			{
				mExtendedPct = max(mExtendedPct - frameTime / getExtendRetractTime(), getMaxRetractedPercentage());
				return mExtendedPct <= getMaxRetractedPercentage() ? ControllableActionStatus::CompletedSuccess : ControllableActionStatus::InProgress;
			}

		default:
			throw UnhandledException(action.type, "ControllableActionType");
		}
	}

	ControllableActionStatus ExtensibleObject::useImpl(Controller* controller, ControllableActionCallback callback)
	{
		auto actionId = handleAction(ControllableActionType::Toggle, true, {}, callback);

		return actionId != ~0u ? getAction(actionId).status : ControllableActionStatus::Unhandled;
	}

} // core