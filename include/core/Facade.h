#pragma once

#include <cstdint>
#include <string>

#include "core/Background.h"
#include "core/Location.h"


namespace core
{

	// A Facade is an occupiable Location whose perimeter walls are all open.
	// It hosts objects and agents exactly as a Room does, owns walkable floor,
	// and takes part in the Graph; the only differences from a Room are that
	// every wall end on every level is open by construction and that it is
	// rendered as a solid opaque colour, like a Background (ADR 0003).
	//
	// "All walls open" is a type invariant, not an editable state: wall
	// add/remove commands and Bulkhead Doors refuse a Facade, and replay
	// constructs every end open intrinsically, so no RemoveWall record is
	// ever emitted for one. A Facade with a wall would semantically be a
	// Room, so the edit is refused rather than allowed to blur the type.
	//
	// World::addFacade() is the creation path: a Facade is placed on any
	// one Layer over a block of free cells with the same placement validation
	// a Room plays by, and is persisted as a ConstructionType::Facade record
	// carrying its name, footprint, and packed colour.
	class Facade : public Location
	{
		BackgroundColour mColour;

	public:

		// The name a Facade carries when nothing authors one. Persistence keeps
		// it explicit in the record, so a hand-authored record that leaves the
		// name out and an unnamed addFacade() call land on the same Sector.
		static std::string defaultName();

		// A muted sand colour, chosen to be distinct from the Background
		// (96,128,160), the Fore (192,192,255) and Back (224,224,255)
		// Location colours, and the lights-off tint.
		static BackgroundColour defaultColour();

		Facade(std::string const& name, uint32_t layerIndex, uint32_t index,
			uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t levelsHigh,
			float topLevelHeight, BackgroundColour const& colour = defaultColour());

		~Facade() = default;

		[[nodiscard]] BackgroundColour getColour() const;

		void setColour(BackgroundColour const& colour);

		// Overridden from Sector
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Location. A Facade hosts everything a Room hosts,
		// except that a Bulkhead Door has no wall ends to be set into: a
		// Facade has none.
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;
	};

} // core
