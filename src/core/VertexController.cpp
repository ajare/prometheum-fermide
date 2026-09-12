#include "core/VertexController.h"
#include "core/SectorObject.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	VertexController::VertexController(SectorObject const* owner, shared_ptr<VertexControllerArea> area, InterSectorExitType interSectorExitType, float interSectorExitOffset)
		: mwOwner(owner)
		, mAreas{ area, nullptr }
		, mInterSectorExitType(interSectorExitType)
		, mInterSectorExitOffset(interSectorExitOffset)
		, mExitingAgentArea(-1)
	{
	}

	VertexController::VertexController(SectorObject const* owner, shared_ptr<VertexControllerArea> area0, shared_ptr<VertexControllerArea> area1, InterSectorExitType interSectorExitType, float interSectorExitOffset)
		: mwOwner(owner)
		, mAreas{ area0, area1 }
		, mInterSectorExitType(interSectorExitType)
		, mInterSectorExitOffset(interSectorExitOffset)
		, mExitingAgentArea(-1)
	{
	}

	SectorObject const* VertexController::getOwner() const
	{
		return mwOwner;
	}

	bool VertexController::hasArea(uint32_t index) const
	{
		assert(index < MaxAreaCount && "index out of bounds.");
		
		return mAreas[index] != nullptr;
	}

	shared_ptr<VertexControllerArea> VertexController::getArea(uint32_t index) const
	{
		assert(index < MaxAreaCount && "index out of bounds.");
		assert(mAreas[index] != nullptr && "no Area at index");

		return mAreas[index];
	}

	Shape const& VertexController::getControlArea(uint32_t index) const
	{
		return getArea(index)->getControlArea();
	}

	bool VertexController::hasInterLayerExitArea(uint32_t index) const
	{
		return getArea(index)->hasExitArea();
	}

	Shape const& VertexController::getInterLayerExitArea(uint32_t index) const
	{
		return getArea(index)->getExitArea();
	}

	bool VertexController::inControlArea(SectorPosition const& pos, uint32_t* index) const
	{
		auto layerIndex = pos.sector()->getLayerIndex();
		auto area = getArea(layerIndex);

		if (area->inControlArea(pos.global()))
		{
			*index = layerIndex;
			return true;
		}
		{
			*index = ~0u;
			return false;
		}
	}

	VertexController::InterSectorExitType VertexController::getInterSectorExitType() const
	{
		return mInterSectorExitType;
	}

	float VertexController::getInterSectorExitOffset() const
	{
		return mInterSectorExitOffset;
	}

	bool VertexController::transitionRequiresControl(shared_ptr<const Vertex> fromVertex, shared_ptr<const Vertex> toVertex) const
	{
		if (toVertex)
		{
			auto fromSector = fromVertex->getSector();
			auto toSector = toVertex->getSector();

			return fromSector->getLayerIndex() != toSector->getLayerIndex() ||
				fromSector->getCellY() != toSector->getCellY();
		}
		else
		{
			return false;
		}
	}

	bool VertexController::controllerUseRequired() const
	{
		return false;
	}

	void VertexController::registerAgentForControl(Agent* agent, uint32_t index)
	{
		auto area = getArea(index);

		area->registerAgentForControl(agent);
	}

	void VertexController::updateAreas(float frameTime)
	{
		for (int i = 0; i < MaxAreaCount; ++i)
		{
			if (mAreas[i])
			{
				mAreas[i]->update(frameTime);
			}
		}
	}

	void VertexController::updateImpl(float frameTime)
	{
	}

	void VertexController::onAgentExitAreaCallback(Agent* agent)
	{
		mExitingAgentArea = -1;
	}

	void VertexController::update(float frameTime)
	{
		updateAreas(frameTime);

		// If there is an Agent ready to exit in an Area, then tell it to do so
		if (mExitingAgentArea == -1)
		{
			if (mAreas[0]->hasAgentReadyToExit())
			{
				mAreas[0]->requestAgentToExit(bind(&VertexController::onAgentExitAreaCallback, this, placeholders::_1));
				mExitingAgentArea = 0;
			}
		}

		updateImpl(frameTime);
	}

} // core
