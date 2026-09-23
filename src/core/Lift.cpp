#include <ranges>
#include <format>

#include "core/Defines.h"
#include "core/Lift.h"
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

	std::vector<CellPosition> liftStops(uint32_t cellX, uint32_t cellY, vector<uint32_t> stopOffsets)
	{
		return to_vector(views::transform(stopOffsets, [cellX, cellY](uint32_t offset)
		{
			return CellPosition(cellX, cellY + offset);
		}));
	}

	/***

	Lift
	----

	A Lift spans multiple, potentially non-contiguous Levels, either within a Location, or on the Back Layer, connecting
	separate Locations on the Fore Layer.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- stopOffsets is list of offsets relative to cellY, not global.
	*/
	Lift::Lift(uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, float transportWidth, float transportHeight, float speed, vector<uint32_t> stopOffsets)
		: RailedTransport(xOffset, yOffset, transportWidth, transportHeight, speed, liftStops(cellX, cellY, stopOffsets), false)
	{
	}

} // core