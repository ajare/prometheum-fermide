#include "core/Vector2.h"


namespace core
{

	const Vector2 Vector2::ZERO(0, 0);
	const Vector2 Vector2::UNIT_X(1, 0);
	const Vector2 Vector2::UNIT_Y(0, 1);
	const Vector2 Vector2::NEGATIVE_UNIT_X(-1, 0);
	const Vector2 Vector2::NEGATIVE_UNIT_Y(0, -1);

} // core


core::Vector2 operator*(float value, core::Vector2 const& vec)
{
	return vec * value;
}