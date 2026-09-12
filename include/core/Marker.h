#pragma once

#include <cstdint>
#include <memory>

#include "core/Object.h"
#include "core/Vector2.h"


namespace core
{

	class Marker : public Object
	{
		uint32_t mCellX, mCellY;

		float mOffset;

	private:

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback) override;

	public:

		Marker(uint32_t cellX, uint32_t cellY, float xOffset);

		uint32_t getCellX() const;

		uint32_t getCellY() const;

		float getOffset() const;

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;
	};

} // core
