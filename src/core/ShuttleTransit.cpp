#include <ranges>

#include "core/Defines.h"
#include "core/ShuttleTransit.h"
#include "core/ShuttleEdge.h"
#include "core/Shuttle.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	// We need this to explicitly convert our crazy list comprehension to a vector
	template <ranges::range R>
	constexpr auto to_vector(R&& r)
	{
		using elem_t = decay_t<ranges::range_value_t<R>>;
		return vector<elem_t>{ r.begin(), r.end() };
	}

	ShuttleTransit::ShuttleTransit(uint32_t index, uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t numCars, uint32_t carWidth, vector<TransitStop> const& stops)
		: Transit(SectorType::Shuttle, "Shuttle", CORE_LAYER_BACK, index,
			cellX, cellY,
			0.0f, 0.0f,
			(float)cellsWide, CORE_SHUTTLE_HEIGHT,
			cellsWide, 1,
			CORE_SHUTTLE_HEIGHT,
			~0u,
			stops)
		, VerticalEdgeCreator()
	{
		vector<uint32_t> stopOffsets = to_vector(views::transform(stops, [cellX](auto const& stop)
		{
			return (uint32_t)(((int)stop.sector->getCellX() + stop.sectorOffsetX) - (int)cellX);
		}));

		mShuttle = make_shared<Shuttle>(cellX, cellY, 0.0f, 0.0f, (float)cellsWide, CORE_SHUTTLE_CAR_HEIGHT, numCars, carWidth, stopOffsets);
	}

	shared_ptr<Shuttle> ShuttleTransit::getShuttle() const
	{
		return mShuttle;
	}

	string ShuttleTransit::getDescription() const
	{
		return mShuttle->getDescription();
	}

	bool ShuttleTransit::sectorSupportsObjectType(SectorObjectType type) const
	{
		return type == SectorObjectType::Door
			|| type == SectorObjectType::Window;
	}

	shared_ptr<Edge> ShuttleTransit::createCrossDeckEdge(shared_ptr<VerticalEdgeCreator> edgeCreator) const
	{
		ASSERT_PTR_EQ_THIS(edgeCreator);

		return make_shared<ShuttleEdge>(mShuttle);
	}

	void ShuttleTransit::updateImpl(float frameTime)
	{
		mShuttle->update(frameTime);
	}

} // core