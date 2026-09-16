#include <format>

#include "core/Defines.h"
#include "core/Staircase.h"

namespace core
{
	using namespace std;

	Staircase::Staircase(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, int riseSide)
		: Object((float)cellX, (float)cellY, (float)cellsWide, 2.0f)
		, mCellsWide(cellsWide)
		, mRiseSide(riseSide)
	{
		ASSERT_SIDE_OK(riseSide);
	}

	array<Vector2, 2> Staircase::getPath() const
	{
		float const left = 0.5f;
		float const right = (float)mCellsWide - 0.5f;
		return mRiseSide == CORE_SIDE_RIGHT
			? array<Vector2, 2>{ Vector2{ left, 0.0f }, Vector2{ right, 1.0f } }
			: array<Vector2, 2>{ Vector2{ right, 0.0f }, Vector2{ left, 1.0f } };
	}

	string Staircase::getDescription() const
	{
		return format("Staircase - {} cells wide, rising {}", mCellsWide,
			mRiseSide == CORE_SIDE_RIGHT ? "right" : "left");
	}

	void Staircase::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}
}
