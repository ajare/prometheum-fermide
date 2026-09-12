#pragma once

#include <cstdint>
#include <memory>

#include "core/Defines.h"
#include "core/OpenableObject.h"
#include "core/Coordination.h"


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

		DoorActivationMode mActivationMode{ DoorActivationMode::Manual };

		TraversalResourceId mTraversalResource;

		float mHoldOpenTime{ CORE_DOOR_STAY_OPEN_TIME };

		uint32_t mOpenLeaseCount{ 0 };

		bool mObstructed{ false };

	private:

		// Overridden from Controllable
		bool validateAction(ControllableActionType type) const override;

		bool modifyAndReject(ControllableActionType type, ControllableActionData const& data) override;

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

		[[nodiscard]] DoorActivationMode getActivationMode() const { return mActivationMode; }

		[[nodiscard]] TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }

		void configureTraversal(DoorActivationMode mode, TraversalResourceId resource, float holdOpenTime);

		void acquireOpenLease();

		void releaseOpenLease();

		[[nodiscard]] uint32_t getOpenLeaseCount() const { return mOpenLeaseCount; }

		void setObstructed(bool obstructed) { mObstructed = obstructed; }

		[[nodiscard]] bool isObstructed() const { return mObstructed; }

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
