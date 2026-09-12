#pragma once

#include <cstdint>
#include <memory>

#include "core/Defines.h"
#include "core/OpenableObject.h"


namespace core
{
	class Sector;

	class Door : public OpenableObject
	{
		friend class Building;

	public:

		enum struct OpenStyle
		{
			VertFromFloor,
			HorzFromCentre,
			QuadIris
		};

	private:

		uint32_t mCellsWide;

		OpenStyle mOpenStyle;

		std::shared_ptr<const Sector> mSectors[2];

	private:

		// Overridden from Controllable
		bool validateAction(ControllableActionType type) const override;

		// Overridden from Controllable
		void updateImpl(float frameTime, ControllableActionType action, ControllableActionStatus status) override;

	protected:

		ControllableActionStatus startAction(ControllableAction const& action) override;

		void finishAction(ControllableAction const& action) override;

		ControllableActionStatus updateAction(ControllableAction const& action, float frameTime) override;

	public:

		Door(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, std::shared_ptr<const Sector> sectors[2]);

		~Door() = default;

		[[nodiscard]] uint32_t getCellsWide() const;

		[[nodiscard]] OpenStyle getOpenStyle() const;

		[[nodiscard]] std::shared_ptr<const Sector> getSector(uint32_t layerIndex) const;

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
