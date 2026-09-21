#include "core/AgentTag.h"

#include <utility>

namespace core
{
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
