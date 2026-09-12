#include <cassert>

#include "core/Defines.h"
#include "core/OpenableObject.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Openable Object
	---------------

	This is an intermediate class which implements logic for objects which can opened, for instance Doors.
	*/
	OpenableObject::OpenableObject(float x, float y, float width, float height)
		: Object(x, y, width, height)
		, mState(State::Closed)
		, mOpenPct(0.0f)
		, mOpenWaitTime(0.0f)
	{
	}

	OpenableObject::State const& OpenableObject::getState() const
	{
		return mState;
	}

	float OpenableObject::getOpenPercentage() const
	{
		return mOpenPct;
	}

	float OpenableObject::getOpenWaitTime() const
	{
		return mOpenWaitTime;
	}

	bool OpenableObject::isOpen() const
	{
		return mState == State::Open;
	}

	bool OpenableObject::isClosed() const
	{
		return mState == State::Closed;
	}

	bool OpenableObject::isOpening() const
	{
		return mState == State::Opening;
	}

	bool OpenableObject::isClosing() const
	{
		return mState == State::Closing;
	}

	bool OpenableObject::open()
	{
		if (mState != State::Open)
		{
			mState = State::Opening;
		}

		return true;
	}

	bool OpenableObject::close()
	{
		if (mState != State::Closed)
		{
			mState = State::Closing;
		}

		return true;
	}

	bool OpenableObject::toggle()
	{
		switch (mState)
		{
		case State::Open:
		case State::Opening:
			return close();

		case State::Closed:
		case State::Closing:
			return open();

		default:
			throw UnhandledException(mState, "OpenableObject::State");
		}
	}

	ControllableActionStatus OpenableObject::useImpl(Controller* controller, ControllableActionCallback callback)
	{
		auto actionId = handleAction(ControllableActionType::Open, true, {}, callback);

		return actionId != ~0u ? getAction(actionId).status : ControllableActionStatus::Unhandled;
	}

} // core