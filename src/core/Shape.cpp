#include "core/Shape.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	Shape
	-----

	This is a base class used by classes like SectorObject.
	*/

	Shape::Shape(float x, float y, float width, float height)
		: mPosition(x, y)
		, mSize(width, height)
	{
	}

	Shape::Shape(Vector2 const& pos, Vector2 const& size)
		: mPosition(pos)
		, mSize(size)
	{
	}

	Vector2 Shape::getPosition() const
	{
		return mPosition;
	}

	Vector2 const& Shape::getSize() const
	{
		return mSize;
	}

	void Shape::getFullShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		minExtent = getPosition();
		maxExtent = minExtent + getSize();
	}

	void Shape::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}

	void Shape::setPosition(Vector2 const& position)
	{
		mPosition = position;
	}

	bool Shape::pointInShape(Vector2 const& pos) const
	{
		return pointInShape(pos.x, pos.y);
	}

	bool Shape::pointInShape(float x, float y) const
	{
		Vector2 minExtent, maxExtent;

		getFullShape(minExtent, maxExtent);

		if (x < minExtent.x)
		{
			return false;
		}
		if (y < minExtent.y)
		{
			return false;
		}
		if (x > maxExtent.x)
		{
			return false;
		}
		if (y > maxExtent.y)
		{
			return false;
		}

		return true;
	}

} // core