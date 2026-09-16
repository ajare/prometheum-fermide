#pragma once

#include <array>
#include <cstdint>

#include "core/Object.h"

namespace core
{
	// A single straight flight connecting two adjacent decks.
	class Staircase : public Object
	{
		uint32_t mCellsWide;
		int mRiseSide;

	public:
		Staircase(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, int riseSide);
		~Staircase() = default;

		[[nodiscard]] uint32_t getCellsWide() const { return mCellsWide; }
		[[nodiscard]] int getRiseSide() const { return mRiseSide; }
		[[nodiscard]] uint32_t getStepCount() const { return mCellsWide * 8; }

		// Local coordinates, from the lower corridor endpoint to the upper one.
		[[nodiscard]] std::array<Vector2, 2> getPath() const;

		[[nodiscard]] std::string getDescription() const override;
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};
}
