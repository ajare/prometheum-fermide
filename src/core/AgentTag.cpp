#include "core/AgentTag.h"

#include <cmath>
#include <utility>

namespace core
{
	void agentColourToFloats(AgentColour const& colour, float out[3])
	{
		out[0] = static_cast<float>(colour.r) / 255.0f;
		out[1] = static_cast<float>(colour.g) / 255.0f;
		out[2] = static_cast<float>(colour.b) / 255.0f;
	}

	AgentColour agentColourFromFloats(float const in[3])
	{
		auto toByte = [](float value) -> uint8_t
		{
			if (!(value > 0.0f)) return 0;
			if (value > 1.0f) return 255;
			return static_cast<uint8_t>(std::lround(value * 255.0f));
		};
		return { toByte(in[0]), toByte(in[1]), toByte(in[2]) };
	}

	std::unique_ptr<AgentTag> AgentTag::create(std::string name)
	{
		return std::unique_ptr<AgentTag>(new AgentTag(std::move(name)));
	}

	bool AgentTag::nameIsValid(std::string const& name, std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (name.empty()) return reject("An Agent tag name must contain at least one character");
		if (name.size() > MaxNameCharacters)
		{
			return reject("An Agent tag name cannot exceed "
				+ std::to_string(MaxNameCharacters) + " characters");
		}

		bool previousWasHyphen{ true };
		for (auto const character : name)
		{
			auto const alphanumeric = (character >= 'a' && character <= 'z')
				|| (character >= '0' && character <= '9');
			if (alphanumeric)
			{
				previousWasHyphen = false;
				continue;
			}
			if (character != '-' || previousWasHyphen)
			{
				return reject("An Agent tag name must match "
					"[a-z0-9]+(?:-[a-z0-9]+)* and must not include #");
			}
			previousWasHyphen = true;
		}
		if (previousWasHyphen)
		{
			return reject("An Agent tag name must match "
				"[a-z0-9]+(?:-[a-z0-9]+)* and must not include #");
		}

		if (diagnostic) diagnostic->clear();
		return true;
	}
}
