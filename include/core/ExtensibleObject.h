#pragma once

#include <cstdint>
#include <set>

#include "core/Object.h"
#include "core/EntityId.h"

namespace core
{
	class ExtensibleObject : public Object
	{
	public:
		enum struct State { Extended, Extending, Retracted, Retracting };

	protected:
		State mState;
		bool mIsExtensible;
		float mExtendedPct;
		uint32_t mExtensionLeaseCount{ 0 };
		std::set<SectorId> mExtensionControlSectors;

	public:
		ExtensibleObject(float x, float y, float width, float height,
			bool extensible, bool startExtended);
		bool isExtensible() const;
		State const& getState() const;
		float getExtendedPercentage() const;
		virtual float getMaxRetractedPercentage() const;
		virtual float getExtendRetractTime() const = 0;
		bool isExtended() const;
		bool isRetracted() const;
		bool isExtending() const;
		bool isRetracting() const;
		bool extend();
		bool retract();
		bool toggle();
		void update(float frameTime) override;

		void acquireExtensionLease() { ++mExtensionLeaseCount; }
		bool releaseExtensionLease();
		uint32_t getExtensionLeaseCount() const { return mExtensionLeaseCount; }
		void addExtensionControlSector(SectorId sector);
		bool canPrepareFrom(SectorId sector) const;
		bool hasExtensionControl() const { return !mExtensionControlSectors.empty(); }
	};
}
