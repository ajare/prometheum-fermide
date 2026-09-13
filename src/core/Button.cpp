#include "core/Defines.h"
#include "core/Button.h"

namespace core
{
	Button::Button(std::string const& name, uint32_t cellX, uint32_t cellY,
		float xOffset, float yOffset, uint32_t flags)
		: Object((float)cellX + xOffset, (float)cellY + yOffset,
			CORE_BUTTON_SIZE, CORE_BUTTON_SIZE / CORE_CELL_YX_RENDER_RATIO)
		, mName(name)
		, mAutoReEnable((flags & CORE_BUTTON_F_AUTO_REENABLE) != 0)
	{
	}

	std::string Button::getDescription() const { return mName; }
	void Button::enable() { mEnabled = true; mEnableTimer = -1.0f; }

	void Button::disable()
	{
		mEnabled = false;
		if (mAutoReEnable) mEnableTimer = CORE_BUTTON_DISABLE_TIME;
	}

	void Button::update(float frameTime)
	{
		if (!mEnabled && mAutoReEnable && (mEnableTimer -= frameTime) <= 0.0f)
			enable();
	}

	void Button::_setPlacement(float centerX, float baseY, float yAdjustment)
	{
		setPosition({ centerX - getSize().x * 0.5f, baseY + yAdjustment });
	}
}
