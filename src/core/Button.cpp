#include <cassert>

#include "core/Defines.h"
#include "core/Button.h"
#include "core/ControllerVertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Button 
	------

	A Button is a small object placed somewhere in a Location, use to interact with another object.

	Construction arguments:

	- cellX and cellY are global, not relative to the Sector that it's in.
	- xOffset and yOffset are offsets to cellX and cellY to precisely place the Button
	*/
	Button::Button(string const& name, uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, uint32_t flags)
		: Object((float)cellX + xOffset, (float)cellY + yOffset, CORE_BUTTON_SIZE, CORE_BUTTON_SIZE / CORE_CELL_YX_RENDER_RATIO)
		, Controller()
		, mName(name)
		, mEnableTimer(-1.0f)
		, mAutoReEnable((flags & CORE_BUTTON_F_AUTO_REENABLE) != 0)
	{
	}

	string Button::getDescription() const
	{
		return mName;
	}

	ControllableActionStatus Button::press(Controller* subject)
	{
		auto res = sendActionToOrchestrator(core::ControllerActionType::Press, subject);

		switch (res)
		{
		case ControllableActionStatus::InProgress:
		case ControllableActionStatus::CompletedSuccess:
			Button::disable();
			break;

		case ControllableActionStatus::Unhandled:
		case ControllableActionStatus::Rejected:
			break;

		default:
			break;
		}

		return res;
	}

	ControllableActionStatus Button::useImpl(Controller* subject, ControllableActionCallback callback)
	{
		return press(subject);
	}

	bool Button::canBeUsed(Controller const* controller) const
	{
		return Controller::canBeUsed(controller);
	}

	void Button::disable()
	{
		Useable::disable();

		if (mAutoReEnable)
		{
			mEnableTimer = CORE_BUTTON_DISABLE_TIME;
		}
	}

	void Button::updateImpl(float frameTime, ControllableActionType action, ControllableActionStatus status)
	{
		if (mAutoReEnable)
		{
			if (!Useable::isEnabled())
			{
				mEnableTimer -= frameTime;

				if (mEnableTimer <= 0.0f)
				{
					Useable::enable();
				}
			}
		}
	}

	void Button::_adjustY(float delta)
	{
		auto pos = getPosition();

		pos.y += delta;

		setPosition(pos);
	}

} // core