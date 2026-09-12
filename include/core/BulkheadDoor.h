#pragma once

#include <cstdint>
#include <memory>

#include "core/OpenableObject.h"
#include "core/DependentPathControllable.h"


namespace core
{

	class Sector;

	class BulkheadDoor : public OpenableObject
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

		// Overridden from Controllable
		bool validateAction(ControllableActionType type) const override;

		// Overriden from Controllable
		void updateImpl(float frameTime, ControllableActionType action, ControllableActionStatus status) override;

	protected:

		// Overriden from Controllable
		ControllableActionStatus startAction(ControllableAction const& action) override;

		// Overriden from Controllable
		void finishAction(ControllableAction const& action) override;

		// Overriden from Controllable
		ControllableActionStatus updateAction(ControllableAction const& action, float frameTime) override;

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
