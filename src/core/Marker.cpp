#include <cassert>
#include <utility>

#include "core/Defines.h"
#include "core/Marker.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Marker
	-------

	Markers are essentially floating platforms within Locations.  They must be placed in a Location with height greater
	than 1, and on a level greater than zero.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- cellsWide should generally be 1, but in theory there's no reason why it can't be any value greater than zero.
	*/
	Marker::Marker(MarkerId id, string name, uint32_t cellX, uint32_t cellY, float xOffset)
		: Object((float)cellX, (float)cellY, 1.0f, 0.0f)
		, mId(id)
		, mName(std::move(name))
		, mCellX(cellX)
		, mCellY(cellY)
		, mOffset(xOffset)
	{
	}

	string Marker::trimName(string const& value)
	{
		auto const first = value.find_first_not_of(" \t");
		if (first == string::npos) return {};
		auto const last = value.find_last_not_of(" \t");
		return value.substr(first, last - first + 1);
	}

	bool Marker::nameIsValid(string const& trimmed, string* diagnostic)
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (trimmed.empty()) return reject("A Marker name cannot be blank");
		if (trimmed.size() > MaxNameBytes)
			return reject("A Marker name cannot exceed " + to_string(MaxNameBytes)
				+ " bytes (got " + to_string(trimmed.size()) + ")");

		auto const* bytes = reinterpret_cast<unsigned char const*>(trimmed.data());
		size_t index = 0;
		while (index < trimmed.size())
		{
			auto const lead = bytes[index];
			if (lead == 0) return reject("A Marker name cannot contain a NUL byte");
			size_t continuation = 0;
			unsigned int minimum = 0;
			if (lead < 0x80) { ++index; continue; }
			if ((lead & 0xE0) == 0xC0) { continuation = 1; minimum = 0x80; }
			else if ((lead & 0xF0) == 0xE0) { continuation = 2; minimum = 0x800; }
			else if ((lead & 0xF8) == 0xF0) { continuation = 3; minimum = 0x10000; }
			else return reject("A Marker name must be valid UTF-8");
			if (index + continuation >= trimmed.size())
				return reject("A Marker name must be valid UTF-8");
			unsigned int codepoint = lead & (0xFF >> (continuation + 1));
			for (size_t i = 1; i <= continuation; ++i)
			{
				auto const next = bytes[index + i];
				if ((next & 0xC0) != 0x80) return reject("A Marker name must be valid UTF-8");
				codepoint = (codepoint << 6) | (next & 0x3F);
			}
			if (codepoint < minimum || codepoint > 0x10FFFF
				|| (codepoint >= 0xD800 && codepoint <= 0xDFFF))
				return reject("A Marker name must be valid UTF-8");
			index += continuation + 1;
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	/***

	getCellX()
	----------

	Get global cell x-position.
	*/
	uint32_t Marker::getCellX() const
	{
		return mCellX;
	}

	/***

	getCellY()
	----------

	Get global cell y-position.
	*/
	uint32_t Marker::getCellY() const
	{
		return mCellY;
	}

	float Marker::getOffset() const
	{
		return mOffset;
	}

	std::string Marker::getDescription() const
	{
		return "Marker";
	}


} // core