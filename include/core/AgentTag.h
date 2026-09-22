#pragma once

#include <array>
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

	// The sixteen pastel Colours that intrinsic tag display Colours draw from.
	// Draws need not be distinct; each draw is uniform and independent.
	inline constexpr std::array<AgentColour, 16> AgentTagColourPalette
	{
		AgentColour{ 255, 179, 186 }, // pastel pink
		AgentColour{ 255, 205, 178 }, // pastel peach
		AgentColour{ 255, 223, 186 }, // pastel apricot
		AgentColour{ 255, 239, 186 }, // pastel yellow
		AgentColour{ 240, 255, 190 }, // pastel lime-yellow
		AgentColour{ 202, 255, 191 }, // pastel lime
		AgentColour{ 186, 255, 201 }, // pastel mint
		AgentColour{ 186, 255, 235 }, // pastel aqua
		AgentColour{ 186, 225, 255 }, // pastel sky
		AgentColour{ 195, 198, 255 }, // pastel periwinkle
		AgentColour{ 222, 194, 255 }, // pastel lavender
		AgentColour{ 240, 190, 255 }, // pastel orchid
		AgentColour{ 255, 196, 246 }, // pastel fuchsia
		AgentColour{ 255, 214, 214 }, // pastel rose
		AgentColour{ 226, 213, 198 }, // pastel sand
		AgentColour{ 205, 218, 205 }, // pastel sage
	};

	struct AgentColourProperty
	{
		AgentColour value{};
		uint64_t revision{ 0 };

		bool operator==(AgentColourProperty const& other) const = default;
	};

	struct AgentModifierRange
	{
		float minimum{ 1.0f };
		float maximum{ 1.0f };

		bool operator==(AgentModifierRange const& other) const = default;
	};

	inline constexpr float AgentWalkSpeedModifierMinimum{ 0.8f };
	inline constexpr float AgentWalkSpeedModifierMaximum{ 1.2f };
	inline constexpr AgentModifierRange DefaultAgentWalkSpeedModifierRange{};
	inline constexpr float AgentHeightModifierMinimum{ 0.7f };
	inline constexpr float AgentHeightModifierMaximum{ 1.0f };
	inline constexpr AgentModifierRange DefaultAgentHeightModifierRange{};

	struct AgentWalkSpeedModifierProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentWalkSpeedModifierProperty const& other) const = default;
	};

	struct AgentHeightModifierProperty
	{
		AgentModifierRange range{};
		uint64_t revision{ 0 };

		bool operator==(AgentHeightModifierProperty const& other) const = default;
	};

	void agentColourToFloats(AgentColour const& colour, float out[3]);
	AgentColour agentColourFromFloats(float const in[3]);

	// Draws one pastel from AgentTagColourPalette uniformly at random. Tag
	// creation and legacy-load backfill share this so automatic Colours behave
	// identically. The generator is never consulted by load or simulation reset.
	AgentColour sampleAgentTagColour();
	bool agentWalkSpeedModifierRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	bool agentHeightModifierRangeIsValid(AgentModifierRange const& range,
		std::string* diagnostic = nullptr);
	float sampleAgentModifier(AgentModifierRange const& range);

	// A named reusable set of Agent properties. Property types are hardcoded;
	// the optional values and their revisions are authored registry data.
	class AgentTag
	{
		friend class AgentTagRegistry;

		std::string mName;
		// The intrinsic display Colour used when rendering the tag itself (for
		// example its Selection-panel chip). It is deliberately separate from
		// the optional Agent Colour property below, which Agents inherit.
		AgentColour mDisplayColour{};
		std::optional<AgentColourProperty> mColour;
		std::optional<AgentWalkSpeedModifierProperty> mWalkSpeedModifier;
		std::optional<AgentHeightModifierProperty> mHeightModifier;

		explicit AgentTag(std::string name)
			: mName(std::move(name))
		{
		}

		void setName(std::string name) { mName = std::move(name); }
		void setDisplayColour(AgentColour colour) { mDisplayColour = colour; }
		void setColour(AgentColourProperty colour) { mColour = colour; }
		void removeColour() { mColour.reset(); }
		void setWalkSpeedModifier(AgentWalkSpeedModifierProperty property)
		{
			mWalkSpeedModifier = property;
		}
		void removeWalkSpeedModifier() { mWalkSpeedModifier.reset(); }
		void setHeightModifier(AgentHeightModifierProperty property)
		{
			mHeightModifier = property;
		}
		void removeHeightModifier() { mHeightModifier.reset(); }

	public:
		static constexpr size_t MaxNameCharacters{ 12 };

		static std::unique_ptr<AgentTag> create(std::string name);
		static bool nameIsValid(std::string const& name, std::string* diagnostic = nullptr);

		std::string const& getName() const { return mName; }
		AgentColour getDisplayColour() const { return mDisplayColour; }
		AgentColourProperty const* getColour() const
		{
			return mColour ? &*mColour : nullptr;
		}
		AgentWalkSpeedModifierProperty const* getWalkSpeedModifier() const
		{
			return mWalkSpeedModifier ? &*mWalkSpeedModifier : nullptr;
		}
		AgentHeightModifierProperty const* getHeightModifier() const
		{
			return mHeightModifier ? &*mHeightModifier : nullptr;
		}
	};
}
