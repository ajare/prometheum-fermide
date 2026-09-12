#include <algorithm>

#include "core/Defines.h"
#include "core/RailedTransport.h"
#include "core/Door.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	RailedTransport
	---------------

	This is the base class for various Lifts and Shuttles.

	Current stop is modelled as a float.  This lets us use the stop as the position.  By default, we always
	start at rest at the first Stop in the list.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- decksHigh is the number of decks that the RailedTransport spans.  So a RailedTransport joining Decks 0 and 1 will have a decksHigh of 2
	*/
	RailedTransport::RailedTransport(float xOffset, float yOffset, float transportWidth, float transportHeight, float speed, vector<CellPosition> const& stops, bool looping)
		: Object((float)stops[0].x + xOffset, (float)stops[0].y + yOffset, transportWidth, transportHeight)
		, mState(State::Idle)
		, mStops(stops)
		, mLooping(looping)
		, mSpeed(speed)
		, mStopTimer(-1.0f)
		, mCurStop(0.0f)
		, mPrevStop(0.0f)
		, mMoveDir(0)
	{
	}

	vector<pair<string, string>> RailedTransport::getInternalsStrings() const
	{
		// Add in defaults
		auto res = Object::getInternalsStrings();

		string stateStr;

		switch (mState)
		{
		case State::Idle:
			stateStr = "Idle";
			break;

		case State::Moving:
			stateStr = "Moving";
			break;

		case State::Arrived:
			stateStr = "Arrived";
			break;

		case State::WaitingOpenAndDisembark:
			stateStr = "WaitingOpenAndDisembark";
			break;

		case State::WaitingEmbarkAndClose:
			stateStr = "WaitingEmbarkAndClose";
			break;

		case State::WaitingDisembark:
			stateStr = "WaitingDisembark";
			break;

		case State::WaitingEmbark:
			stateStr = "WaitingEmbark";
			break;

		case State::Leaving:
			stateStr = "Leaving";
			break;

		default:
			stateStr = "?";
			break;
		}
		
		res.push_back({ "Transit State", stateStr });
		res.push_back({ "Local Position", to_string(mCurStop) });
		res.push_back({ "Direction", to_string(mMoveDir) });
		res.push_back({ "Stop Timer", to_string(mStopTimer) });
		
		return res;
	}

	bool RailedTransport::hasStop(uint32_t x, uint32_t y) const
	{
		return getStopIndex(x, y) != ~0u;
	}

	uint32_t RailedTransport::getNumStops() const
	{
		return (uint32_t)mStops.size();
	}

	CellPosition const& RailedTransport::getStop(uint32_t index) const
	{
		assert(index <= getNumStops());

		return mStops.at(index);
	}

	float RailedTransport::getStopDistance(uint32_t index) const
	{
		assert(index <= getNumStops());

		return mStops[0].distance(mStops[index]);
	}

	uint32_t RailedTransport::getStopDeckIndex(uint32_t index) const
	{
		assert(index <= getNumStops());

		return mStops[index].y;
	}

	uint32_t RailedTransport::getStopIndex(uint32_t x, uint32_t y) const
	{
		for (uint32_t i = 0; i < getNumStops(); ++i)
		{
			if (mStops[i].x == x && mStops[i].y == y)
			{
				return i;
			}
		}

		return ~0u;
	}

	Vector2 RailedTransport::getPosition(uint32_t* lowStopIndex) const
	{
		float dist{ 0.0f };
		uint32_t i = 0;

		if (mCurStop < 0.0f)
		{
			if (lowStopIndex)
			{
				*lowStopIndex = mMoveDir > 0 ? ~0u : 0;
			}

			return { (float)mStops[0].x, (float)mStops[0].y };
		}

		while (i < (getNumStops() - 1))
		{
			auto const& s0 = mStops[i];
			auto const& s1 = mStops[i + 1];

			auto ds = s0.distance(s1);

			if ((dist + ds) > mCurStop)
			{
				float d = mCurStop - dist;
				float dt = d / ds;

				// Position is s0.lerp(s1, dt)
				float px = s0.x + ((float)s1.x - (float)s0.x) * dt;
				float py = s0.y + ((float)s1.y - (float)s0.y) * dt;

				if (lowStopIndex)
				{
					*lowStopIndex = mMoveDir > 0 ? i : i + 1;
				}

				return { px, py };
			}

			dist += ds;
			i++;
		}

		auto cp = getStop(i);

		if (lowStopIndex)
		{
			*lowStopIndex = mMoveDir > 0 ? i : i + 1;
		}

		return { (float)cp.x, (float)cp.y };
	}

	vector<uint32_t> RailedTransport::getRequestedStops() const
	{
		vector<uint32_t> stops{};

		for (auto const& action : mActions)
		{
			if (action.type == ControllableActionType::CallToStop)
			{
				stops.push_back(getStopRefIndex((uint32_t)action.data.i));
			}
		}

		reverse(stops.begin(), stops.end());
		return stops;
	}

	void RailedTransport::arriveAtStop(uint32_t index)
	{
		mCurStop = mMoveDir > 0 ? floorf(mCurStop) : ceilf(mCurStop);
		mMoveDir = 0;
		mState = State::Arrived;
		mStopTimer = CORE_LIFT_DOOR_PAUSE_TIME;
	}

	void RailedTransport::updatePosition()
	{
		setPosition(getPosition());
	}

	ControllableActionStatus RailedTransport::useImpl(Controller* controller, ControllableActionCallback callbac)
	{
		return ControllableActionStatus::Unhandled;
	}

	void RailedTransport::sortActionsByPriority(vector<ControllableAction>& actions)
	{
		// Here, we want to use the standard "elevator algorithm".
		// Find the closest stop in the direction we are moving,
		// ie the one with the smallest delta to current position.
		// Logic is reversed because we want the first entry at the
		// end of the vector.
		float curStop = mCurStop;
		float moveDir = curStop - mPrevStop;

		auto c = [this, curStop, moveDir](auto const& a, auto const& b)
		{
			auto distA = this->getStopDistance(a.data.i);
			auto distB = this->getStopDistance(b.data.i);

			int signMove = moveDir >= 0.0f ? 1 : -1;
			int signA = distA >= curStop ? 1 : -1;
			int signB = distB >= curStop ? 1 : -1;

			if (signA > 0 && signB > 0)
			{
				return (a.data.i - curStop) > (b.data.i - curStop);
			}
			else if (signA < 0 && signB < 0)
			{
				return (a.data.i - curStop) < (b.data.i - curStop);
			}
			else
			{
				return signA != signMove;
			}
		};

		sort(actions.begin(), actions.end(), c);
	}

	bool RailedTransport::interruptAction(ControllableActionType newAction, ControllableActionType curAction)
	{
		return false;
	}

	bool RailedTransport::modifyAndReject(ControllableActionType type, ControllableActionData const& data)
	{
		switch (type)
		{
		case ControllableActionType::CallToStop:
			// Are we already at the stop?
			if (mCurStop == mStops[0].distance(mStops[data.i]))
			{
				if (mState == State::WaitingEmbark || mState == State::WaitingEmbarkAndClose)
				{
					// If agents are embarking, then nothing to do: the door will not close
					// until the new requester has embarked.
					// If the door is closing, then revert back to "arrived" state, ie triggering
					// door to open. At this point, the action will have completed, and any new action
					// will be pending, to be kicked off once
				}
				return true;
			}
			else
			{
				return false;
			}
			break;

		default:
			return false;
		}
	}

	bool RailedTransport::validateAction(ControllableActionType type) const
	{
		return type == ControllableActionType::CallToStop;
	}

	ControllableActionStatus RailedTransport::startAction(ControllableAction const& action)
	{
		switch (action.type)
		{
		case ControllableActionType::CallToStop:
			mMoveDir = action.data.i - mCurStop == 0.0f ? 0 : (action.data.i - mCurStop > 0.0f ? 1 : -1);
			
			if (mMoveDir != 0)
			{
				mState = State::Moving;
			}

			return ControllableActionStatus::InProgress;

		default:
			throw UnhandledException(action.type, "ControllableActionType");
		}
	}

	ControllableActionStatus RailedTransport::updateCallToStop(ControllableAction const& action, float frameTime)
	{
		if (mState == State::Arrived)
		{
			mStopTimer -= frameTime;
			
			if (mStopTimer <= 0.0f)
			{
				mState = State::WaitingOpenAndDisembark;
				return ControllableActionStatus::CompletedSuccess;
			}
			else
			{
				return ControllableActionStatus::InProgress;
			}
		}
		else if (mState == State::Leaving)
		{
			mStopTimer -= frameTime;

			if (mStopTimer <= 0.0f)
			{
				mState = State::Moving;
				return ControllableActionStatus::InProgress;
			}
			else
			{
				return ControllableActionStatus::Paused;
			}
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

	ControllableActionStatus RailedTransport::updateAction(ControllableAction const& action, float frameTime)
	{
		switch (action.type)
		{
		case ControllableActionType::CallToStop:
			return updateCallToStop(action, frameTime);

		default:
			throw UnhandledException(action.type, "ControllableActionType");
		}
	}

	void RailedTransport::onDoorOpened(Door const* door)
	{
		addLogMessage(getDescription(), getId(), LogLevel::Debug, "ON: door opened");
		mState = State::WaitingEmbarkAndClose;
	}

	void RailedTransport::onDoorClosed(Door const* door)
	{
		addLogMessage(getDescription(), getId(), LogLevel::Debug, "ON: door closed");

		if (!mActions.empty())
		{
			auto const& action = getCurrentAction();

			mMoveDir = action.data.i - mCurStop == 0.0f ? 0 : (action.data.i - mCurStop > 0.0f ? 1 : -1);

			if (mMoveDir != 0)
			{
				mState = State::Leaving;
				mStopTimer = CORE_LIFT_DOOR_PAUSE_TIME;
			}
		}
		else
		{
			mState = State::Idle;
		}
	}

} // core