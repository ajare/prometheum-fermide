#include <format>

#include "core/Defines.h"
#include "core/PlatformLift.h"


namespace core
{

	using namespace std;

	/***

	PlatformLift
	------------

	PlatformLift is a subclass of Lift which is used to connect floors of a Location.  It is intended to be open,
	unlike a CarLift

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- stopOffsets is list of offsets relative to cellY, not global.
	*/
	PlatformLift::PlatformLift(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, vector<uint32_t> const& stopOffsets)
		: Lift(cellX, cellY, 0.0f, 0.0f, (float)cellsWide, -0.05f, CORE_LIFT_SPEED, stopOffsets)
	{
	}

	string PlatformLift::getDescription() const
	{
		return "Platform Lift";
	}

	/***

	getCurrentShape()
	-----------------

	Get the current, physical extents of the Lift.
	*/
	void PlatformLift::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}

	void PlatformLift::arriveAtStop(uint32_t index)
	{
		mCurStop = mMoveDir > 0 ? floorf(mCurStop) : ceilf(mCurStop);
		mMoveDir = 0;
		mState = State::Arrived;
	}

	ControllableActionStatus PlatformLift::updateCallToStop(ControllableAction const& action, float frameTime)
	{
		if (mState == State::Arrived)
		{
			mState = State::WaitingDisembark;
			return ControllableActionStatus::CompletedSuccess;
		}
		else if (mState == State::WaitingDisembark)
		{
			mState = State::WaitingEmbark;
			return ControllableActionStatus::InProgress;
		}
		else if (mState == State::WaitingEmbark)
		{
			if (!mActions.empty())
			{
				auto const& action = getCurrentAction();

				mMoveDir = action.data.i - mCurStop == 0.0f ? 0 : (action.data.i - mCurStop > 0.0f ? 1 : -1);

				if (mMoveDir != 0)
				{
					mState = State::Leaving;
				}
			}
			else
			{
				mState = State::Idle;
			}

			return ControllableActionStatus::InProgress;
		}
		else if (mState == State::Leaving)
		{
			mState = State::Moving;
			return ControllableActionStatus::InProgress;
		}
		else if (mState == State::Moving)
		{
			float move = mSpeed * mMoveDir * frameTime;
			uint32_t stopIndex0, stopIndex1;

			auto oldStop = mCurStop;
			getPosition(&stopIndex0);

			// Only update previous position if we're moving.  This is so
			// we know which direction we came from, even if we are stopped
			// at a deck.
			if (move != 0.0f)
			{
				mPrevStop = mCurStop;
			}

			mCurStop += move;
			getPosition(&stopIndex1);

			// Have we passed a stop we care about?
			if (stopIndex0 != stopIndex1 && (uint32_t)getCurrentAction().data.i == stopIndex1)
			{
				arriveAtStop(stopIndex1);
			}

			updatePosition();

			return ControllableActionStatus::InProgress;
		}
		else
		{
			return action.status;
		}
	}

} // core