#pragma once

#include <cmath>


namespace core
{

	struct CellPosition
	{
		uint32_t x, y;

	public:

		float distance(CellPosition const& other) const
		{
			float fx0 = (float)x;
			float fy0 = (float)y;
			float fx1 = (float)other.x;
			float fy1 = (float)other.y;

			return sqrtf((fx0 - fx1) * (fx0 - fx1) + (fy0 - fy1) * (fy0 - fy1));
		}
	};

} // core
