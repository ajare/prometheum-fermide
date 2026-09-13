#pragma once

#include <cstdint>
#include <memory>

#include "core/ExtensibleObject.h"
#include "core/EntityId.h"


namespace core
{

	class ForceBridge : public ExtensibleObject
	{
		friend class Building;

	private:

		int mFromSide;

		TraversalResourceId mTraversalResource;

	public:

		ForceBridge(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, int fromSide, bool extensible, bool startExtended);

		~ForceBridge() = default;

		[[nodiscard]] int getFromSide() const;

		[[nodiscard]] TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }

		void configureTraversal(TraversalResourceId resource) { mTraversalResource = resource; }

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from SectorObject
		[[nodiscard]] float getExtendRetractTime() const override;

		// Overridden from SectorObject
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};

} // core
