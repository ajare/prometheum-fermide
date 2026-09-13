#pragma once

#include <cstdint>
#include <memory>
#include <set>

#include "core/Object.h"
#include "core/EntityId.h"


namespace core
{

	class ExtensibleObject : public Object
	{
	public:

		enum struct State
		{
			Extended,
			Extending,
			Retracted,
			Retracting
		};

		State mState;

		bool mIsExtensible;

		float mExtendedPct;

		uint32_t mExtensionLeaseCount{ 0 };

		std::set<SectorId> mExtensionControlSectors;

	private:

		// Overridden from Controllable
		bool validateAction(ControllableActionType type) const override;

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback) override;

	protected:

		// Overriden from Controllable
		ControllableActionStatus startAction(ControllableAction const& action) override;

		// Overriden from Controllable
		void finishAction(ControllableAction const& action) override;

		// Overriden from Controllable
		ControllableActionStatus updateAction(ControllableAction const& action, float frameTime) override;

	public:

		ExtensibleObject(float x, float y, float width, float height, bool extensible, bool startExtended);

		[[nodiscard]] bool isExtensible() const;

		[[nodiscard]] State const& getState() const;

		[[nodiscard]] float getExtendedPercentage() const;

		[[nodiscard]] virtual float getMaxRetractedPercentage() const;

		[[nodiscard]] virtual float getExtendRetractTime() const = 0;

		[[nodiscard]] bool isExtended() const;

		[[nodiscard]] bool isRetracted() const;

		[[nodiscard]] bool isExtending() const;

		[[nodiscard]] bool isRetracting() const;

		// Overridden from Useable
		bool canBeUsed(Controller const* controller) const override;

		bool extend();

		bool retract();

		bool toggle();

		void acquireExtensionLease() { ++mExtensionLeaseCount; }

		bool releaseExtensionLease()
		{
			if (mExtensionLeaseCount == 0) return false;
			--mExtensionLeaseCount;
			return true;
		}

		[[nodiscard]] uint32_t getExtensionLeaseCount() const { return mExtensionLeaseCount; }


		void addExtensionControlSector(SectorId sector) { if (sector) mExtensionControlSectors.insert(sector); }

		[[nodiscard]] bool canPrepareFrom(SectorId sector) const
		{
			return isExtended() || !isExtensible() || mExtensionControlSectors.contains(sector);
		}

		[[nodiscard]] bool hasExtensionControl() const { return !mExtensionControlSectors.empty(); }
	};

} // core
