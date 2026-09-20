// Agent group naming rules; see include/core/AgentGroup.h.

#include "core/AgentGroup.h"

#include <cstdint>
#include <string>

namespace core
{
	std::unique_ptr<AgentGroup> AgentGroup::create(std::string name)
	{
		// Not std::make_unique: that free function is not a friend, and the
		// constructor deliberately is not public.
		return std::unique_ptr<AgentGroup>(new AgentGroup(std::move(name)));
	}

	std::string AgentGroup::trimName(std::string const& value)
	{
		auto const first = value.find_first_not_of(" \t");
		if (first == std::string::npos) return {};
		auto const last = value.find_last_not_of(" \t");
		return value.substr(first, last - first + 1);
	}

	bool AgentGroup::nameIsValid(std::string const& trimmed, std::string* diagnostic)
	{
		auto reject = [&](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (trimmed.empty())
			return reject("An Agent group name cannot be blank");
		if (trimmed.size() > MaxNameBytes)
			return reject("An Agent group name cannot exceed "
				+ std::to_string(MaxNameBytes) + " bytes (got "
				+ std::to_string(trimmed.size()) + ")");

		// The name is stored and written as UTF-8, so it is checked as UTF-8.
		// A malformed sequence would survive validation here only to be
		// rejected - or mangled - by whatever reads the file next, and an
		// embedded NUL would truncate the name at every C-string boundary.
		auto const bytes = reinterpret_cast<unsigned char const*>(trimmed.data());
		size_t index{ 0 };
		while (index < trimmed.size())
		{
			auto const lead = bytes[index];
			if (lead == 0x00)
				return reject("An Agent group name cannot contain a NUL byte");

			size_t continuation{ 0 };
			unsigned int minimum{ 0 };
			if (lead < 0x80)
			{
				++index;
				continue;
			}
			else if ((lead & 0xE0) == 0xC0)
			{
				continuation = 1;
				minimum = 0x80;
			}
			else if ((lead & 0xF0) == 0xE0)
			{
				continuation = 2;
				minimum = 0x800;
			}
			else if ((lead & 0xF8) == 0xF0)
			{
				continuation = 3;
				minimum = 0x10000;
			}
			else
			{
				return reject("An Agent group name must be valid UTF-8");
			}

			if (index + continuation >= trimmed.size())
				return reject("An Agent group name must be valid UTF-8");

			unsigned int codepoint = lead & (0xFF >> (continuation + 1));
			for (size_t i = 1; i <= continuation; ++i)
			{
				auto const next = bytes[index + i];
				if ((next & 0xC0) != 0x80)
					return reject("An Agent group name must be valid UTF-8");
				codepoint = (codepoint << 6) | (next & 0x3F);
			}

			// Overlong encodings and the surrogate range are not valid UTF-8
			// even though their byte shapes pass the tests above.
			if (codepoint < minimum || codepoint > 0x10FFFF
				|| (codepoint >= 0xD800 && codepoint <= 0xDFFF))
				return reject("An Agent group name must be valid UTF-8");

			index += continuation + 1;
		}

		if (diagnostic) diagnostic->clear();
		return true;
	}

} // core
