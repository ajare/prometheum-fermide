#pragma once

#include <cstdint>
#include <memory>

#include "core/Door.h"
#include "core/DependentPathControllable.h"


namespace core
{

	class Sector;

	class BulkheadDoor : public Door
	{
		friend class Building;

	public:

		enum struct OpenStyle
		{
			VertFromFloor
		};

	private:

		OpenStyle mOpenStyle;

		std::shared_ptr<const Sector> mSectors[2];

	public:

		BulkheadDoor(uint32_t cellX, uint32_t cellY, std::shared_ptr<const Sector> sectors[2]);

		~BulkheadDoor() = default;

		[[nodiscard]] OpenStyle getOpenStyle() const;

		[[nodiscard]] std::shared_ptr<const Sector> getSideSector(int side) const;
		
		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from OpenableObject
		[[nodiscard]] float getOpenCloseTime() const override;

		// Overridden from OpenableObject
		float getTimeBeforeClosing() const override;

		// Overridden from OpenableObject
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};

} // core
