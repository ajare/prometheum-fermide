#pragma once

#include <cstdint>
#include <memory>

#include "core/Object.h"


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
	};

} // core
