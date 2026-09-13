#pragma once

#include <cstdint>
#include <memory>

#include "core/Object.h"
#include "core/EntityId.h"


namespace core
{

	class Staircase : public Object
	{
		uint32_t mDecksHigh;

		int mMountSide;

		TraversalResourceId mTraversalResource;

	private:

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback) override;

	public:

		Staircase(uint32_t cellX, uint32_t cellY, uint32_t decksHigh, int mountSide);

		~Staircase() = default;

		[[nodiscard]] uint32_t getDecksHigh() const;

		[[nodiscard]] int getMountSide() const;

		[[nodiscard]] TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }

		void configureTraversal(TraversalResourceId resource) { mTraversalResource = resource; }

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Shape
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};

} // core
