#include <ranges>

#include "core/Defines.h"
#include "core/LiftTransit.h"
#include "core/LiftEdge.h"
#include "core/CarLift.h"
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

	LiftTransit::LiftTransit(uint32_t index, uint32_t layerIndex, uint32_t cellX, uint32_t cellY, uint32_t cellsWide,
		uint32_t decksHigh, vector<TransitStop> const& stops)
		: Transit(SectorType::Lift, "Lift", layerIndex, index,
			cellX, cellY,
			0.0f, 0.0f,
			(float)cellsWide, (float)decksHigh,
			cellsWide, decksHigh,
			1.0f,
			~0u,
			stops)
		, VerticalEdgeCreator()
	{
		vector<uint32_t> stopOffsets = to_vector(views::transform(stops, [cellY](auto const& stop)
		{
			return (uint32_t)((int)stop.sector->getCellY() + stop.sectorOffsetY - (int)cellY);
		}));

		mLift = make_shared<CarLift>(cellX, cellY, cellsWide, stopOffsets);
	}

	shared_ptr<Lift> LiftTransit::getLift() const
	{
		return mLift;
	}

	string LiftTransit::getDescription() const
	{
		return mLift->getDescription();
	}

	bool LiftTransit::sectorSupportsObjectType(SectorObjectType type) const
	{
		return type == SectorObjectType::Door 
			|| type == SectorObjectType::Window;
	}

	shared_ptr<Edge> LiftTransit::createCrossDeckEdge([[maybe_unused]] shared_ptr<VerticalEdgeCreator> edgeCreator) const
	{
		ASSERT_PTR_EQ_THIS(edgeCreator);

		return make_shared<LiftEdge>(mLift);
	}

	void LiftTransit::updateImpl(float frameTime)
	{
		mLift->update(frameTime);
	}

} // core