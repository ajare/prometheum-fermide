#include <algorithm>

#include "core/ExtensibleObject.h"
#include "core/Exceptions.h"

namespace core
{
	ExtensibleObject::ExtensibleObject(float x, float y, float width, float height,
		bool extensible, bool startExtended)
		: Object(x, y, width, height),
		  mState(startExtended ? State::Extended : State::Retracted),
		  mIsExtensible(extensible), mExtendedPct(startExtended ? 1.0f : 0.0f)
	{
	}

	bool ExtensibleObject::isExtensible() const { return mIsExtensible; }
	ExtensibleObject::State const& ExtensibleObject::getState() const { return mState; }
	float ExtensibleObject::getExtendedPercentage() const { return mExtendedPct; }
	float ExtensibleObject::getMaxRetractedPercentage() const { return 0.0f; }
	bool ExtensibleObject::isExtended() const { return mState == State::Extended; }
	bool ExtensibleObject::isRetracted() const { return mState == State::Retracted; }
	bool ExtensibleObject::isExtending() const { return mState == State::Extending; }
	bool ExtensibleObject::isRetracting() const { return mState == State::Retracting; }

	bool ExtensibleObject::extend()
	{
		if (!mIsExtensible) return false;
		if (!isExtended()) mState = State::Extending;
		return true;
	}

	bool ExtensibleObject::retract()
	{
		if (!mIsExtensible || mExtensionLeaseCount != 0) return false;
		if (!isRetracted()) mState = State::Retracting;
		return true;
	}

	bool ExtensibleObject::toggle()
	{
		if (!mIsExtensible) return false;
		return isExtended() || isExtending() ? retract() : extend();
	}

	void ExtensibleObject::update(float frameTime)
	{
		if (isExtending())
		{
			mExtendedPct = std::min(mExtendedPct + frameTime / getExtendRetractTime(), 1.0f);
			if (mExtendedPct >= 1.0f) { mExtendedPct = 1.0f; mState = State::Extended; }
		}
		else if (isRetracting())
		{
			if (mExtensionLeaseCount != 0) { mState = State::Extending; return; }
			mExtendedPct = std::max(mExtendedPct - frameTime / getExtendRetractTime(),
				getMaxRetractedPercentage());
			if (mExtendedPct <= getMaxRetractedPercentage())
				mState = State::Retracted;
		}
	}

	bool ExtensibleObject::releaseExtensionLease()
	{
		if (mExtensionLeaseCount == 0) return false;
		--mExtensionLeaseCount;
		return true;
	}

	void ExtensibleObject::addExtensionControlSector(SectorId sector)
	{
		if (sector) mExtensionControlSectors.insert(sector);
	}

	bool ExtensibleObject::canPrepareFrom(SectorId sector) const
	{
		return isExtended() || !isExtensible() || mExtensionControlSectors.contains(sector);
	}
}
