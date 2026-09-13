#pragma once

#include "core/Object.h"

namespace core
{
	class OpenableObject : public Object
	{
	public:
		enum struct State { Open, Opening, Closed, Closing };

	protected:
		State mState{ State::Closed };
		float mOpenPct{ 0.0f };
		float mOpenWaitTime{ 0.0f };

	public:
		OpenableObject(float x, float y, float width, float height);
		State const& getState() const;
		float getOpenPercentage() const;
		float getOpenWaitTime() const;
		virtual float getOpenCloseTime() const = 0;
		virtual float getTimeBeforeClosing() const = 0;
		bool isOpen() const;
		bool isClosed() const;
		bool isOpening() const;
		bool isClosing() const;
		bool open();
		bool close();
		bool toggle();
	};
}
