#include <cmath>
#include <format>

#include "core/Defines.h"
#include "core/Staircase.h"

namespace core
{
	using namespace std;

	Staircase::Staircase(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, int riseSide,
		float speed)
		: Object((float)cellX, (float)cellY, (float)cellsWide, 2.0f)
		, mCellsWide(cellsWide)
		, mRiseSide(riseSide)
		, mSpeed(speed)
	{
		ASSERT_SIDE_OK(riseSide);
	}

	array<Vector2, 2> Staircase::getPath() const
	{
		// Span the complete authored footprint: the lower endpoint starts at the
		// outside edge of its cell and the upper endpoint reaches the outside edge
		// of the opposite cell.
		float const left = 0.0f;
		float const right = (float)mCellsWide;
		return mRiseSide == CORE_SIDE_RIGHT
			? array<Vector2, 2>{ Vector2{ left, 0.0f }, Vector2{ right, 1.0f } }
			: array<Vector2, 2>{ Vector2{ right, 0.0f }, Vector2{ left, 1.0f } };
	}

	void Staircase::update(float frameTime)
	{
		if (!isEscalator()) return;
		auto const path = getPath();
		float const length = path[0].distanceTo(path[1]);
		mAnimationPhase = fmod(mAnimationPhase + mSpeed * frameTime / length, 1.0f);
		if (mAnimationPhase < 0.0f) mAnimationPhase += 1.0f;
	}

	string Staircase::getDescription() const
	{
		if (isEscalator())
			return format("Escalator - {} cells wide, moving {} at {}", mCellsWide,
				mSpeed > 0.0f ? "up" : "down", abs(mSpeed));
		return format("Staircase - {} cells wide, rising {}", mCellsWide,
			mRiseSide == CORE_SIDE_RIGHT ? "right" : "left");
	}

	void Staircase::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}
}
