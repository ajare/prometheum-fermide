#pragma once

#include <string>
#include <memory>

#include "core/Vector2.h"
#include "core/VerticalEdgeCreator.h"


namespace core
{

	// Intended as a base class for Interactables, Windows, etc.
	class Shape
	{
		Vector2 mPosition;

		Vector2 mSize;

	protected:

		void setPosition(Vector2 const& position);
		void setSize(Vector2 const& size) { mSize = size; }

	public:

		Shape(float x, float y, float width, float height);

		Shape(Vector2 const& pos, Vector2 const& size);

		virtual ~Shape() = default;

		[[nodiscard]] Vector2 getPosition() const;

		[[nodiscard]] Vector2 const& getSize() const;

		void getFullShape(Vector2& minExtent, Vector2& maxExtent) const;

		virtual void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const;

		[[nodiscard]] bool pointInShape(Vector2 const& pos) const;

		[[nodiscard]] bool pointInShape(float x, float y) const;
	};

} // core
