#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "core/Object.h"
#include "core/EntityId.h"


namespace core
{

	class Stairwell : public Object
	{
		uint32_t mDecksHigh;

		int mMountSide;

		TraversalResourceId mTraversalResource;

	private:


	public:

		Stairwell(uint32_t cellX, uint32_t cellY, uint32_t decksHigh, int mountSide);

		~Stairwell() = default;

		[[nodiscard]] uint32_t getDecksHigh() const;

		[[nodiscard]] int getMountSide() const;

		// Local pathing/render geometry for one connection to the next deck.
		[[nodiscard]] std::array<Vector2, 4> getDeckPath(uint32_t deckOffset) const;

		[[nodiscard]] TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }

		void configureTraversal(TraversalResourceId resource) { mTraversalResource = resource; }

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Shape
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};

} // core
