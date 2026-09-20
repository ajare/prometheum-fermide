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
		// The authored visual manner in which the Door's leaf reveals its threshold.
		// Opening style never changes timing, state, obstruction, or traversal.
		enum struct OpenStyle { OpenUp, OpenLeft };

	private:
		uint32_t mCellsWide;
		OpenStyle mOpenStyle{ OpenStyle::OpenUp };
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
		void setOpenStyle(OpenStyle style);

		// A Door joins exactly one adjacent Layer pair.  The index is the side of that
		// pair, not an absolute Layer index: 0 is the front Layer the Door is authored
		// on, 1 is the Layer directly behind it.
		std::shared_ptr<const Sector> getSector(uint32_t pairSide) const;
		std::shared_ptr<const Sector> getFrontSector() const { return mSectors[0]; }
		std::shared_ptr<const Sector> getBackSector() const { return mSectors[1]; }
		// The absolute Layers the Door crosses.  ~0u when a side has no Sector.
		uint32_t getFrontLayer() const;
		uint32_t getBackLayer() const;
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
