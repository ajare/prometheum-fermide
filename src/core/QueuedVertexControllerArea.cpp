#include <algorithm>

#include "core/QueuedVertexControllerArea.h"
#include "core/Building.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	QueuedVertexControllerArea::QueuedVertexControllerArea(VertexController* owner, Shape const& controlArea, Building const* building, shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t width)
		: VertexControllerArea(owner, controlArea)
	{
		setSpotOffsets(calculateDoorQueueStopOffsets(building, vertex, x, y, width));
	}

	QueuedVertexControllerArea::QueuedVertexControllerArea(VertexController* owner, Shape const& controlArea, Shape const& exitArea, Building const* building, shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t width)
		: VertexControllerArea(owner, controlArea, exitArea)
	{
		setSpotOffsets(calculateDoorQueueStopOffsets(building, vertex, x, y, width));
	}

	QueuedVertexControllerArea::QueuedVertexControllerArea(VertexController* owner, Shape const& controlArea, Building const* building, shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t width, shared_ptr<Controller> controller, shared_ptr<Vertex> controllerVertex)
		: VertexControllerArea(owner, controlArea, controller, controllerVertex)
	{
		setSpotOffsets(calculateDoorQueueStopOffsets(building, vertex, x, y, width));
	}

	QueuedVertexControllerArea::QueuedVertexControllerArea(VertexController* owner, Shape const& controlArea, Shape const& exitArea, Building const* building, shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t width, shared_ptr<Controller> controller, shared_ptr<Vertex> controllerVertex)
		: VertexControllerArea(owner, controlArea, exitArea, controller, controllerVertex)
	{
		setSpotOffsets(calculateDoorQueueStopOffsets(building, vertex, x, y, width));
	}

	vector<float> QueuedVertexControllerArea::calculateDoorQueueStopOffsets(Building const* building, shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t width) const
	{
		vector<float> stops;
		const float stopWidth = CORE_DOOR_QUEUE_STOP_WIDTH;

		auto sector = vertex->getSector();
		auto layer = building->getLayer(sector->getLayerIndex());

		int32_t ix0 = (int32_t)x;
		int32_t ix1 = (int32_t)(x + width);

		// Left extent
		for (; ix0 >= (int32_t)sector->getCellX0(); --ix0)
		{
			auto const& cellDef = layer->getCellDefinition((uint32_t)ix0, y);

			if (!cellDef.isTraversableOnFoot())
			{
				break;
			}
		}

		ix0++;

		// Right extent
		for (; ix1 <= (int32_t)sector->getCellX1(); ++ix1)
		{
			auto const& cellDef = layer->getCellDefinition((uint32_t)ix1, y);

			if (!cellDef.isTraversableOnFoot())
			{
				break;
			}
		}

		ix1--;

		// Generate stops
		stops.push_back(0.0f);

		float vx = vertex->getPosition().x;
		float xc = x + width / 2.0f;
		float stopX = xc - stopWidth;

		while (stopX >= ix0)
		{
			stops.push_back(stopX - vx);
			stopX -= stopWidth;
		}

		stopX = xc + stopWidth;

		while (stopX < (ix1 + 1))
		{
			stops.push_back(stopX - vx);
			stopX += stopWidth;
		}

		// Sort stops by increasing distance from centre of door
		sort(stops.begin(), stops.end(), [](auto a, auto b) { return fabs(a) < fabs(b); });

		return stops;
	}

	void QueuedVertexControllerArea::setSpotOffsets(vector<float> const& offsets)
	{
		for (uint32_t i = 0; i < (uint32_t)offsets.size(); ++i)
		{
			// Each offset's absolute value must be >= the previous one, and the first must be zero
			if (i == 0)
			{
				if (offsets[i] != 0.0f)
				{
					throw Exception("Bad queue offset: index 0 should have value 0.");
				}
			}
			else if (fabs(offsets[i]) < fabs(offsets[i - 1]))
			{
				throw Exception("Bad queue offset: each successive offset cannot be closer to the exit than the previous one.");
			}

			mTargetOffsets.push_back({
				TargetOffsetType::Intermediate,
				offsets[i],
				0.0f,
				nullptr
			});
		}
	}

} // core
