#pragma once

#include <memory>
#include <string>

#include "core/ControllableActionCallback.h"
#include "core/ControllableActionStatus.h"


namespace core
{
	class Controller;

	class Useable
	{
		bool mEnabled;

	private:

		virtual bool isEnabledImpl() const;

		virtual ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback) = 0;

	public:

		Useable();

		virtual ~Useable() = default;

		virtual std::string getDescription() const = 0;

		bool isEnabled() const;

		virtual void enable();

		virtual void disable();

		virtual bool canBeUsed(Controller const* controller) const = 0;

		ControllableActionStatus use(Controller* controller, ControllableActionCallback callback, bool force = false);
	};

} // core

