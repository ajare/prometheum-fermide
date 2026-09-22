#include "core/AgentTag.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
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

	bool agentWalkSpeedModifierRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum))
			return reject("Walk speed modifier endpoints must be finite");
		if (range.minimum < AgentWalkSpeedModifierMinimum
			|| range.minimum > AgentWalkSpeedModifierMaximum
			|| range.maximum < AgentWalkSpeedModifierMinimum
			|| range.maximum > AgentWalkSpeedModifierMaximum)
		{
			return reject("Walk speed modifier endpoints must be between 0.8 and 1.2");
		}
		if (range.minimum > range.maximum)
			return reject("Walk speed modifier minimum cannot exceed its maximum");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool agentHeightModifierRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (!std::isfinite(range.minimum) || !std::isfinite(range.maximum))
			return reject("Height modifier endpoints must be finite");
		if (range.minimum < AgentHeightModifierMinimum
			|| range.minimum > AgentHeightModifierMaximum
			|| range.maximum < AgentHeightModifierMinimum
			|| range.maximum > AgentHeightModifierMaximum)
		{
			return reject("Height modifier endpoints must be between 0.7 and 1.0");
		}
		if (range.minimum > range.maximum)
			return reject("Height modifier minimum cannot exceed its maximum");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	float sampleAgentModifier(AgentModifierRange const& range)
	{
		if (!(range.minimum < range.maximum)) return range.minimum;
		// One engine advances for every sample, so Agents sharing one authored
		// range receive independent draws. Samples themselves are persisted; the
		// generator is never consulted by load or simulation reset.
		static thread_local std::mt19937 engine([]
		{
			std::random_device source;
			std::seed_seq seed{ source(), source(), source(), source(), source(), source() };
			return std::mt19937(seed);
		}());
		std::uniform_real_distribution<float> distribution(range.minimum,
			std::nextafter(range.maximum, std::numeric_limits<float>::max()));
		return std::min(distribution(engine), range.maximum);
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
