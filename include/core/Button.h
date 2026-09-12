#pragma once

#include <cstdint>
#include <memory>

#include "core/Object.h"
#include "core/Controller.h"


namespace core
{

	class Button : public Object, public Controller
	{
		std::string mName;

		float mEnableTimer;

		bool mAutoReEnable;

	private:

		virtual ControllableActionStatus press(Controller* subject);

		// Overridden from Controllable
		void updateImpl(float frameTime, ControllableActionType action, ControllableActionStatus status) override;

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback) override;

	public:

		Button(std::string const& name, uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, uint32_t flags = 0);

		~Button() = default;

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Useable
		bool canBeUsed(Controller const* controller) const override;

		// Overridden from Useable
		void disable() override;

		void _adjustY(float delta);
	};

} // core
