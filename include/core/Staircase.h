#pragma once

#include <cstdint>
#include <memory>

#include "core/Object.h"


namespace core
{

	class Staircase : public Object
	{
		uint32_t mDecksHigh;

		int mMountSide;

	private:

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback) override;

	public:

		Staircase(uint32_t cellX, uint32_t cellY, uint32_t decksHigh, int mountSide);

		~Staircase() = default;

		[[nodiscard]] uint32_t getDecksHigh() const;

		[[nodiscard]] int getMountSide() const;

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Shape
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};

} // core
