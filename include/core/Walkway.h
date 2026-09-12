	#pragma once

#include <cstdint>
#include <memory>

#include "core/Object.h"
#include "core/Vector2.h"


namespace core
{

	class Walkway : public Object
	{
		uint32_t mCellX, mCellY;

	private:

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback) override;

	public:

		Walkway(uint32_t cellX, uint32_t cellY);

		uint32_t getCellX() const;

		uint32_t getCellY() const;

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;
	};

} // core
