#include <format>

#include "core/Defines.h"
#include "core/Facade.h"


namespace core
{

	using namespace std;

	BackgroundColour Facade::defaultColour()
	{
		return BackgroundColour{ 176, 160, 128 };
	}

	Facade::Facade(string const& name, uint32_t layerIndex, uint32_t index,
		uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh,
		float topDeckHeight, BackgroundColour const& colour)
		: Location(name, SectorType::Facade, layerIndex, index, cellX, cellY,
			cellsWide, decksHigh, topDeckHeight, ~0u, false)
		, mColour(colour)
	{
		// The open perimeter is intrinsic to the type: every end on every deck
		// and both sides is created open rather than opened by an edit. No
		// RemoveWall record is ever emitted for a Facade (ADR 0003).
		for (auto& end : mEnds)
		{
			end.end[0] = SectorEndType::None;
			end.end[1] = SectorEndType::None;
		}
	}

	BackgroundColour Facade::getColour() const
	{
		return mColour;
	}

	void Facade::setColour(BackgroundColour const& colour)
	{
		mColour = colour;
	}

	string Facade::getDescription() const
	{
		return format("Facade at {},{} on Layer {}", getCellX(), getCellY(), getLayerIndex());
	}

	bool Facade::sectorSupportsObjectType(SectorObjectType type) const
	{
		// A Facade hosts every object type a Room hosts, with one exception:
		// a Bulkhead Door is set into a pair of wall ends, and a Facade has
		// none. Everything else - Markers, Walkways, Ladders, Lifts, Doors,
		// Windows, controls - follows the Room rule.
		if (type == SectorObjectType::BulkheadDoor)
			return false;

		return Location::sectorSupportsObjectType(type);
	}

} // core
