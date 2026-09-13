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
		enum struct OpenStyle { VertFromFloor, HorzFromCentre, QuadIris };

	private:
		uint32_t mCellsWide;
		OpenStyle mOpenStyle{ OpenStyle::VertFromFloor };
		std::shared_ptr<const Sector> mSectors[2];
		DoorActivationMode mActivationMode{ DoorActivationMode::Manual };
		TraversalResourceId mTraversalResource;
		float mHoldOpenTime{ CORE_DOOR_STAY_OPEN_TIME };
		uint32_t mOpenLeaseCount{ 0 };
		bool mObstructed{ false };

	protected:
		Door(float x, float y, float width, float height, uint32_t cellsWide,
			std::shared_ptr<const Sector> sectors[2]);

	public:
		Door(uint32_t cellX, uint32_t cellY, uint32_t cellsWide,
			std::shared_ptr<const Sector> sectors[2]);

		uint32_t getCellsWide() const;
		OpenStyle getOpenStyle() const;
		std::shared_ptr<const Sector> getSector(uint32_t layerIndex) const;
		DoorActivationMode getActivationMode() const { return mActivationMode; }
		TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }
		void configureTraversal(DoorActivationMode mode, TraversalResourceId resource, float holdOpenTime);
		void acquireOpenLease();
		void releaseOpenLease();
		uint32_t getOpenLeaseCount() const { return mOpenLeaseCount; }
		void setObstructed(bool obstructed) { mObstructed = obstructed; }
		bool isObstructed() const { return mObstructed; }

		// Typed device operations call these commands; no callback/action queue exists.
		bool requestOpen();
		bool requestClose();
		void update(float frameTime) override;

		std::string getDescription() const override;
		float getOpenCloseTime() const override;
		float getTimeBeforeClosing() const override;
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};
}
