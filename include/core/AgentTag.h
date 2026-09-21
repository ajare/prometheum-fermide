#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace core
{
	// A named reusable set of Agent properties. Properties are added by later
	// feature tickets; identity and the canonical user-authored name live here.
	class AgentTag
	{
		friend class AgentTagRegistry;

		std::string mName;

		explicit AgentTag(std::string name)
			: mName(std::move(name))
		{
		}

		void setName(std::string name) { mName = std::move(name); }

	public:
		static constexpr size_t MaxNameCharacters{ 12 };

		static std::unique_ptr<AgentTag> create(std::string name);
		static bool nameIsValid(std::string const& name, std::string* diagnostic = nullptr);

		std::string const& getName() const { return mName; }
	};
}
