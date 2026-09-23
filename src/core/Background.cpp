#include <cmath>
#include <format>

#include "core/Background.h"
#include "core/Defines.h"


namespace core
{

	using namespace std;

	Background::Background(string const& name, uint32_t layerIndex, uint32_t index,
		uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t levelsHigh,
		BackgroundColour const& colour)
		: Sector(SectorType::Background, layerIndex, index, cellX, cellY, 0.0f, 0.0f,
			(float)cellsWide, (float)((levelsHigh - 1) + CORE_ROOM_MAX_HEIGHT), name,
			cellsWide, levelsHigh, CORE_ROOM_MAX_HEIGHT, 0)
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

	uint32_t packBackgroundColour(BackgroundColour const& colour)
	{
		return (uint32_t(colour.r) << 16) | (uint32_t(colour.g) << 8) | uint32_t(colour.b);
	}

	BackgroundColour unpackBackgroundColour(uint32_t packed)
	{
		return BackgroundColour
		{
			(uint8_t)((packed >> 16) & 0xFFu),
			(uint8_t)((packed >> 8) & 0xFFu),
			(uint8_t)(packed & 0xFFu)
		};
	}

	void backgroundColourToFloats(BackgroundColour const& colour, float out[3])
	{
		out[0] = (float)colour.r / 255.0f;
		out[1] = (float)colour.g / 255.0f;
		out[2] = (float)colour.b / 255.0f;
	}

	BackgroundColour backgroundColourFromFloats(float const in[3])
	{
		auto toByte = [](float value) -> uint8_t
		{
			// Written so a NaN fails low rather than escaping as an arbitrary byte.
			if (!(value > 0.0f)) return 0;
			if (value > 1.0f) return 255;
			return (uint8_t)std::lround(value * 255.0f);
		};
		return BackgroundColour{ toByte(in[0]), toByte(in[1]), toByte(in[2]) };
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
