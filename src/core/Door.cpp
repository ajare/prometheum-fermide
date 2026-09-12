#include <cassert>

#include "core/Door.h"
#include "core/DoorVertex.h"
#include "core/Sector.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Door
	----

	This class represents a barrier between two Sectors on different Layers.

	It is slightly different from other SectorObjects in that it does not have a Layer, but
	rather sits between two Layers.

	It generates a Vertex on either side of it, and an Edge between them, and can be controlled
	via an Interactable, or directly opened.  It is either stateful or stateless.  Stateful means
	that when used (either directly or via a button), its state toggles.  Stateless means that when
	used, it tries to open.  Stateless doors will close after a certain time.

	Construction arguments:

	- cellX and cellY are global, not relative to the Sector that it's in.
	- cellsWide should generally be 1, but in theory there's no reason why it can't be any value greater than zero.
	- sectors[2] is the fore and back Sector (see CORE_LAYER_FORE / CORE_LAYER_BACK)
	*/
	Door::Door(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, shared_ptr<const Sector> sectors[2])
		: OpenableObject((float)cellX + CORE_DOOR_X_INSET, (float)cellY, cellsWide - CORE_DOOR_X_INSET * 2.0f, CORE_DOOR_HEIGHT)
		, mCellsWide(cellsWide)
		, mOpenStyle(OpenStyle::VertFromFloor)
		, mSectors{ sectors[0], sectors[1] }
	{
	}

	/***

	getCellsWide()
	--------------

	Get the width of the Door, in cells.
	*/
	uint32_t Door::getCellsWide() const
	{
		return mCellsWide;
	}

	/***

	getOpenStyle()
	--------------

	The OpenStyle is really just visual.  The only thing that matters, functionally is the open state,
	which determines visibility and traversability.
	*/
	Door::OpenStyle Door::getOpenStyle() const
	{
		return mOpenStyle;
	}


	/***

	getSector()
	-----------

	Get the Sector, for the given Layer.
	*/	
	shared_ptr<const Sector> Door::getSector(uint32_t layerIndex) const
	{
		ASSERT_LAYER_OK(layerIndex);

		return mSectors[layerIndex];
	}

	void Door::configureTraversal(DoorActivationMode mode, TraversalResourceId resource, float holdOpenTime)
	{
		mActivationMode = mode;
		mTraversalResource = resource;
		mHoldOpenTime = max(0.0f, holdOpenTime);
	}

	void Door::acquireOpenLease()
	{
		++mOpenLeaseCount;
	}

	void Door::releaseOpenLease()
	{
		if (mOpenLeaseCount == 0)
		{
			return;
		}
		--mOpenLeaseCount;
		if (mOpenLeaseCount == 0 && isOpen())
		{
			mOpenWaitTime = mHoldOpenTime;
		}
	}

	/***

	getDescription()
	----------------

	Get a description.
	*/
	string Door::getDescription() const
	{
		return "Door";
	}

	/***

	getOpenCloseTime()
	------------------

	Get the time taken for the Door to fully open or close.  The time for opening
	and closing will always be the same.
	*/
	float Door::getOpenCloseTime() const
	{
		return CORE_DOOR_OPEN_CLOSE_TIME;
	}

	/***

	getTimeBeforeClosing()
	----------------------

	Doors which have state will automatically close.  This is the time that an open door
	waits before closing.
	*/
	float Door::getTimeBeforeClosing() const
	{
		return mHoldOpenTime;
	}

	/***

	getCurrentShape()
	-----------------

	Get the current, physical extents of the Door.  If the Door is partially
	open, then it will take this into consideration.

	Currently this returns the full shape, as the Door may be rendered in different ways.
	So, the current shape may not be accurate, and should be used as the full extents.
	*/
	void Door::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}

	bool Door::validateAction(ControllableActionType type) const
	{
		return type == ControllableActionType::Open
			|| type == ControllableActionType::Close
			|| type == ControllableActionType::None;
	}

	bool Door::modifyAndReject(ControllableActionType type, ControllableActionData const& data)
	{
		CORE_VAR_UNUSED(data);
		// Safety commands are never queued behind an active crossing. The caller
		// receives rejection and may retry after every independently owned lease
		// and obstruction observation has gone away.
		return type == ControllableActionType::Close && (mOpenLeaseCount != 0 || mObstructed);
	}

	ControllableActionStatus Door::startAction(ControllableAction const& action)
	{
		switch (action.type)
		{
		case ControllableActionType::Open:
			open();
			return ControllableActionStatus::InProgress;

		case ControllableActionType::Close:
			close();
			return ControllableActionStatus::InProgress;

		default:
			throw UnhandledException(action.type, "ControllableActionType");
		}
	}

	void Door::finishAction(ControllableAction const& action)
	{
		switch (action.type)
		{
		case ControllableActionType::Open:
			mOpenPct = 1.0f;
			mOpenWaitTime = getTimeBeforeClosing();
			mState = State::Open;
			break;

		case ControllableActionType::Close:
			mOpenPct = 0.0f;
			mState = State::Closed;
			break;

		default:
			throw UnhandledException(action.type, "ControllableActionType");
		}
	}

	ControllableActionStatus Door::updateAction(ControllableAction const& action, float frameTime)
	{
		switch (action.type)
		{
		case ControllableActionType::Open:
			mOpenPct = min(mOpenPct + frameTime / getOpenCloseTime(), 1.0f);
			return mOpenPct >= 1.0f ? ControllableActionStatus::CompletedSuccess : ControllableActionStatus::InProgress;

		case ControllableActionType::Close:
			mOpenPct = max(mOpenPct - frameTime / getOpenCloseTime(), 0.0f);
			return mOpenPct <= 0.0f ? ControllableActionStatus::CompletedSuccess : ControllableActionStatus::InProgress;

		default:
			throw UnhandledException(action.type, "ControllableActionType");
		}
	}

	void Door::updateImpl(float frameTime, ControllableActionType action, ControllableActionStatus status)
	{
		if (isOpen() && action == ControllableActionType::None)
		{
			mOpenWaitTime -= frameTime;

			if (mOpenWaitTime <= 0.0f && mOpenLeaseCount == 0 && !mObstructed
				&& (mTraversalResource || !canSense(SensorType::AgentBlocking)))
			{
				handleAction(ControllableActionType::Close);
			}
		}
	}

} // core