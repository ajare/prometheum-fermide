#include "core/Useable.h"
#include "core/Controller.h"


namespace core
{
	using namespace std;

	Useable::Useable()
		: mEnabled(true)
	{
	}

	void Useable::enable()
	{
		mEnabled = true;
	}

	void Useable::disable()
	{
		mEnabled = false;
	}

	bool Useable::isEnabledImpl() const
	{
		return true;
	}

	bool Useable::isEnabled() const
	{
		return mEnabled && isEnabledImpl();
	}

	ControllableActionStatus Useable::use(Controller* controller, ControllableActionCallback callback, bool force)
	{
		if (canBeUsed(controller) || force)
		{
			return useImpl(controller, callback);
		}
		else
		{
			return ControllableActionStatus::Rejected;
		}
	}

} // core