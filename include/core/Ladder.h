#pragma once

#include <cstdint>
#include <memory>

#include "core/ExtensibleObject.h"
#include "core/DependentPathControllable.h"


namespace core
{

	class Ladder : public ExtensibleObject
	{
		friend class Building;

	private:

		uint32_t mDecksHigh;

	public:

		Ladder(uint32_t cellX, uint32_t cellY, uint32_t decksHigh, bool extensible, bool startExtended);

		~Ladder() = default;

		[[nodiscard]] uint32_t getDecksHigh() const;

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from ExtensibleObject
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;

		// Overridden from ExtensibleObject
		[[nodiscard]] float getMaxRetractedPercentage() const override;

		// Overridden from ExtensibleObject
		[[nodiscard]] float getExtendRetractTime() const override;
	};

} // core
