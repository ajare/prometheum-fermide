#include <format>

#include "core/Background.h"
#include "core/Defines.h"


namespace core
{

	using namespace std;

	Background::Background(string const& name, uint32_t layerIndex, uint32_t index,
		uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh,
		BackgroundColour const& colour)
		: Sector(SectorType::Background, layerIndex, index, cellX, cellY, 0.0f, 0.0f,
			(float)cellsWide, (float)((decksHigh - 1) + CORE_ROOM_MAX_HEIGHT), name,
			cellsWide, decksHigh, CORE_ROOM_MAX_HEIGHT, 0)
		, mColour(colour)
	{
	}

	BackgroundColour Background::getColour() const
	{
		return mColour;
	}

	void Background::setColour(BackgroundColour const& colour)
	{
		mColour = colour;
	}

	string Background::getDescription() const
	{
		return format("Background at {},{} on Layer {}", getCellX(), getCellY(), getLayerIndex());
	}

	bool Background::sectorSupportsObjectType(SectorObjectType /*type*/) const
	{
		// A Background hosts nothing. Not even a Marker, which would be a goal no
		// agent could ever reach.
		return false;
	}

	bool Background::sectorSupportsObjectAsLookTarget(SectorObjectType type) const
	{
		// A Window may look into a Background. Every other threshold is refused
		// one way or the other; see the umbrella threshold table.
		return type == SectorObjectType::Window;
	}

} // core
