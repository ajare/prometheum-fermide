#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace core
{
	// The first hardcoded Agent property. It deliberately has no alpha channel:
	// ordinary Agents are opaque, and selection replaces this colour entirely.
	struct AgentColour
	{
		uint8_t r{ 179 };
		uint8_t g{ 77 };
		uint8_t b{ 77 };

		bool operator==(AgentColour const& other) const = default;
	};

	inline constexpr AgentColour EditorDefaultAgentColour{};
	inline constexpr AgentColour SelectedAgentColour{ 251, 188, 4 };

	struct AgentColourProperty
	{
		AgentColour value{};
		uint64_t revision{ 0 };

		bool operator==(AgentColourProperty const& other) const = default;
	};

	void agentColourToFloats(AgentColour const& colour, float out[3]);
	AgentColour agentColourFromFloats(float const in[3]);

	// A named reusable set of Agent properties. Property types are hardcoded;
	// the optional values and their revisions are authored registry data.
	class AgentTag
	{
		friend class AgentTagRegistry;

		std::string mName;
		std::optional<AgentColourProperty> mColour;

		explicit AgentTag(std::string name)
			: mName(std::move(name))
		{
		}

		void setName(std::string name) { mName = std::move(name); }
		void setColour(AgentColourProperty colour) { mColour = colour; }
		void removeColour() { mColour.reset(); }

	public:
		static constexpr size_t MaxNameCharacters{ 12 };

		static std::unique_ptr<AgentTag> create(std::string name);
		static bool nameIsValid(std::string const& name, std::string* diagnostic = nullptr);

		std::string const& getName() const { return mName; }
		AgentColourProperty const* getColour() const
		{
			return mColour ? &*mColour : nullptr;
		}
	};
}
