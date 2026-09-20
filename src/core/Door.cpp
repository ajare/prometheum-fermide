#include <algorithm>

#include "core/Door.h"
#include "core/Sector.h"

namespace core
{
	Door::Door(float x, float y, float width, float height, uint32_t cellsWide,
		std::shared_ptr<const Sector> sectors[2])
		: OpenableObject(x, y, width, height), mCellsWide(cellsWide),
		  mSectors{ sectors[0], sectors[1] }
	{
	}

	Door::Door(uint32_t cellX, uint32_t cellY, uint32_t cellsWide,
		std::shared_ptr<const Sector> sectors[2])
		: Door((float)cellX + CORE_DOOR_X_INSET, (float)cellY,
			cellsWide - CORE_DOOR_X_INSET * 2.0f, CORE_DOOR_HEIGHT, cellsWide, sectors)
	{
	}

	uint32_t Door::getCellsWide() const { return mCellsWide; }
	Door::OpenStyle Door::getOpenStyle() const { return mOpenStyle; }
	void Door::setOpenStyle(OpenStyle style) { mOpenStyle = style; }

	std::shared_ptr<const Sector> Door::getSector(uint32_t pairSide) const
	{
		ASSERT_PAIR_SIDE_OK(pairSide);
		return mSectors[pairSide];
	}

	uint32_t Door::getFrontLayer() const
	{
		return mSectors[0] ? mSectors[0]->getLayerIndex() : ~0u;
	}

	uint32_t Door::getBackLayer() const
	{
		return mSectors[1] ? mSectors[1]->getLayerIndex() : ~0u;
	}

	void Door::configureTraversal(DoorActivationMode mode, TraversalResourceId resource,
		float holdOpenTime)
	{
		mActivationMode = mode;
		mTraversalResource = resource;
		mHoldOpenTime = std::max(0.0f, holdOpenTime);
	}

	void Door::acquireOpenLease() { ++mOpenLeaseCount; }

	void Door::releaseOpenLease()
	{
		if (mOpenLeaseCount == 0) return;
		--mOpenLeaseCount;
		if (mOpenLeaseCount == 0 && isOpen()) mOpenWaitTime = mHoldOpenTime;
	}

	bool Door::requestOpen()
	{
		if (isOpen() || isOpening()) return true;
		return open();
	}

	bool Door::requestClose()
	{
		if (mOpenLeaseCount != 0 || mObstructed) return false;
		if (isClosed() || isClosing()) return true;
		return close();
	}

	void Door::update(float frameTime)
	{
		if (isOpening())
		{
			mOpenPct = std::min(mOpenPct + frameTime / getOpenCloseTime(), 1.0f);
			if (mOpenPct >= 1.0f)
			{
				mOpenPct = 1.0f;
				mState = State::Open;
				mOpenWaitTime = getTimeBeforeClosing();
			}
		}
		else if (isClosing())
		{
			if (mOpenLeaseCount != 0 || mObstructed)
			{
				requestOpen();
				return;
			}
			mOpenPct = std::max(mOpenPct - frameTime / getOpenCloseTime(), 0.0f);
			if (mOpenPct <= 0.0f)
			{
				mOpenPct = 0.0f;
				mState = State::Closed;
			}
		}
		else if (isOpen() && mOpenLeaseCount == 0 && !mObstructed)
		{
			mOpenWaitTime -= frameTime;
			if (mOpenWaitTime <= 0.0f) requestClose();
		}
	}

	std::string Door::getDescription() const { return "Door"; }
	float Door::getOpenCloseTime() const { return CORE_DOOR_OPEN_CLOSE_TIME; }
	float Door::getTimeBeforeClosing() const { return mHoldOpenTime; }
	void Door::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}
}
