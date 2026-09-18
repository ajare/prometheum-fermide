#pragma once

#include <cstdint>
#include <string>

#include "core/CellDefinition.h"
#include "core/Sector.h"


namespace core
{

	// An opaque RGB colour. Alpha is deliberately absent: a Background is always
	// fully opaque and is always drawn raw, never tinted by its Layer.
	struct BackgroundColour
	{
		// A desaturated sky blue-grey, clearly distinct from the Fore (192,192,255)
		// and Back (224,224,255) Location colours.
		uint8_t r{ 96 };
		uint8_t g{ 128 };
		uint8_t b{ 160 };

		bool operator==(BackgroundColour const& other) const = default;
	};


	// A Background is a non-occupiable Sector that exists only to be seen: the
	// backdrop behind Windows and other apertures from the Layer in front. It
	// hosts no objects, owns no walkable floor, and is wholly absent from the
	// Graph.
	//
	// Dormant: nothing constructs one yet. Building creation, placement rules
	// and serialisation arrive in later tickets (#30 onwards); until then a
	// Background cannot appear in any Building, so no query, switch or render
	// pass can ever see one.
	class Background : public Sector
	{
		BackgroundColour mColour;

	public:

		// A Background owns no walkable floor. The cells it occupies are stamped
		// CellFloorType::None, so CellDefinition::isTraversableOnFoot() is false
		// for every cell it covers and no agent may ever be placed on it.
		static constexpr CellFloorType cellFloorType()
		{
			return CellFloorType::None;
		}

		// topDeckHeight is fixed at CORE_ROOM_MAX_HEIGHT purely to satisfy the base
		// class. Nothing walks a Background's decks, so exposing a different value
		// would imply behaviour that does not exist.
		Background(std::string const& name, uint32_t layerIndex, uint32_t index,
			uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh,
			BackgroundColour const& colour = {});

		~Background() = default;

		[[nodiscard]] BackgroundColour getColour() const;

		void setColour(BackgroundColour const& colour);

		// Overridden from Sector
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Sector. A Background hosts nothing at all, not even a
		// Marker: a Marker here would be a goal no agent could ever reach.
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;

		// Overridden from Sector. A Window on the Layer in front may look into a
		// Background; nothing else may use one as its far side.
		[[nodiscard]] bool sectorSupportsObjectAsLookTarget(SectorObjectType type) const override;
	};

} // core
