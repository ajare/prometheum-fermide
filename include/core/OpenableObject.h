#pragma once

#include <cstdint>
#include <memory>

#include "core/Object.h"


namespace core
{

	class OpenableObject : public Object
	{
	public:

		enum struct State
		{
			Open,
			Opening,
			Closed,
			Closing
		};

	protected:

		State mState;

		float mOpenPct;

		float mOpenWaitTime;

	private:

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback) override;

	public:

		OpenableObject(float x, float y, float width, float height);

		[[nodiscard]] State const& getState() const;

		[[nodiscard]] float getOpenPercentage() const;

		[[nodiscard]] float getOpenWaitTime() const;

		[[nodiscard]] virtual float getOpenCloseTime() const = 0;

		[[nodiscard]] virtual float getTimeBeforeClosing() const = 0;

		[[nodiscard]] bool isOpen() const;

		[[nodiscard]] bool isClosed() const;

		[[nodiscard]] bool isOpening() const;

		[[nodiscard]] bool isClosing() const;

		bool open();

		bool close();

		bool toggle();
	};

} // core
