#include <ranges>
#include <format>

#include "core/Defines.h"
#include "core/Shuttle.h"


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

	vector<CellPosition> shuttleStops(uint32_t cellX, uint32_t cellY, vector<uint32_t> stopOffsets)
	{
		return to_vector(views::transform(stopOffsets, [cellX, cellY](uint32_t offset)
		{
			return CellPosition(cellX + offset, cellY);
		}));
	}

	/***

	Shuttle
	----

	This is essentially a horizontal Lift in functionality.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- stopOffsets is list of offsets relative to cellX, not global.
	*/
	Shuttle::Shuttle(uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, float transportWidth, float transportHeight, uint32_t numCars, uint32_t carWidth, vector<uint32_t> stopOffsets)
		: RailedTransport(xOffset, yOffset, transportWidth, transportHeight, CORE_SHUTTLE_SPEED, shuttleStops(cellX, cellY, stopOffsets), false)
		, mNumCars(numCars)
		, mCarWidth(carWidth)
	{
	}

	/***

	getDescription()
	----------------

	Get a description.
	*/
	string Shuttle::getDescription() const
	{
		return format("Shuttle");
	}

	uint32_t Shuttle::getNumCars() const
	{
		return mNumCars;
	}

	uint32_t Shuttle::getCarWidth() const
	{
		return mCarWidth;
	}


} // core