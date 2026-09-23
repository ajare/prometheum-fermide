#pragma once

#include <array>
#include <cstdint>

#include "core/Object.h"

namespace core
{
	// A single straight flight connecting two adjacent levels.
	class Staircase : public Object
	{
		uint32_t mCellsWide;
		int mRiseSide;
		float mSpeed;
		float mAnimationPhase{ 0.0f };

	public:
		Staircase(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, int riseSide,
			float speed = 0.0f);
		~Staircase() = default;

		[[nodiscard]] uint32_t getCellsWide() const { return mCellsWide; }
		[[nodiscard]] int getRiseSide() const { return mRiseSide; }
		// Zero is an ordinary staircase; positive moves up and negative moves down.
		[[nodiscard]] float getSpeed() const { return mSpeed; }
		[[nodiscard]] bool isEscalator() const { return mSpeed != 0.0f; }
		[[nodiscard]] uint32_t getStepCount() const { return mCellsWide * 8; }

		// Local coordinates, from the lower corridor endpoint to the upper one.
		[[nodiscard]] std::array<Vector2, 2> getPath() const;

		[[nodiscard]] float getAnimationPhase() const { return mAnimationPhase; }
		void update(float frameTime);

		[[nodiscard]] std::string getDescription() const override;
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};
}
