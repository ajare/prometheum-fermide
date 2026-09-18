// Background Sector type checks, for ticket #29.
//
// A Background is a non-occupiable Sector that exists only to be seen through
// apertures from the Layer in front. This ticket lands it dormant: the type, the
// class, its colour, and a render-safe path exist, but Building offers no way to
// create one, so no Building can ever contain one and no traversal, query or
// render pass can observe it. These checks exercise the Background directly
// rather than through a Building, which is exactly the point - the construction
// path is what arrives later (#30).

#include <array>
#include <cstdint>
#include <string>
#include <stdexcept>

#include "core/Background.h"
#include "core/Building.h"
#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Location.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/SectorType.h"
#include "core/Transit.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// Every SectorObjectType the model knows about. A Background must answer false
	// for all of them as a host, so a new type added here is caught by this check
	// rather than silently becoming hostable in a Background.
	std::array<core::SectorObjectType, 11> const AllSectorObjectTypes{
		core::SectorObjectType::None,
		core::SectorObjectType::BulkheadDoor,
		core::SectorObjectType::Door,
		core::SectorObjectType::ForceBridge,
		core::SectorObjectType::InteractionPoint,
		core::SectorObjectType::Ladder,
		core::SectorObjectType::Lift,
		core::SectorObjectType::Marker,
		core::SectorObjectType::Shuttle,
		core::SectorObjectType::Walkway,
		core::SectorObjectType::Window
	};

	// core::getSectorObjectTypeString() refuses the types it never prints, so the
	// diagnostics here name them directly instead.
	std::string nameOf(core::SectorObjectType type)
	{
		switch (type)
		{
		case core::SectorObjectType::None: return "None";
		case core::SectorObjectType::BulkheadDoor: return "BulkheadDoor";
		case core::SectorObjectType::Door: return "Door";
		case core::SectorObjectType::ForceBridge: return "ForceBridge";
		case core::SectorObjectType::InteractionPoint: return "InteractionPoint";
		case core::SectorObjectType::Ladder: return "Ladder";
		case core::SectorObjectType::Lift: return "Lift";
		case core::SectorObjectType::Marker: return "Marker";
		case core::SectorObjectType::Shuttle: return "Shuttle";
		case core::SectorObjectType::Walkway: return "Walkway";
		case core::SectorObjectType::Window: return "Window";
		}
		return "Unknown";
	}

	core::Background makeBackground(core::BackgroundColour const& colour = {})
	{
		return core::Background("Backdrop", 2, 7, 3, 4, 5, 2, colour);
	}

	// The type is named, and named only as itself.
	void theTypeHasAString()
	{
		require(core::getSectorTypeString(core::SectorType::Background) == "Background",
			"getSectorTypeString() does not name SectorType::Background");
	}

	// A Background is a Sector, and a sibling of Location and Transit rather than
	// either of them.
	void aBackgroundIsItsOwnSectorKind()
	{
		auto const background = makeBackground();
		core::Sector const& asSector = background;

		require(background.getType() == core::SectorType::Background,
			"A Background does not report SectorType::Background");
		require(dynamic_cast<core::Location const*>(&asSector) == nullptr,
			"A Background is reachable as a Location");
		require(dynamic_cast<core::Transit const*>(&asSector) == nullptr,
			"A Background is reachable as a Transit");
		require(background.getDescription().rfind("Background", 0) == 0,
			("A Background's description does not lead with its kind: "
				+ background.getDescription()).c_str());
	}

	// The default colour is the desaturated sky blue-grey from the spec, and the
	// setter round-trips it.
	void theColourDefaultsAndRoundTrips()
	{
		auto background = makeBackground();

		require(background.getColour().r == 96
			&& background.getColour().g == 128
			&& background.getColour().b == 160,
			"The default Background colour is not (96, 128, 160)");

		background.setColour({ 10, 200, 30 });

		require(background.getColour().r == 10
			&& background.getColour().g == 200
			&& background.getColour().b == 30,
			"setColour() did not round-trip");

		// A default-constructed colour is the default colour, so callers that do
		// not care about colour still get the spec default.
		require(core::BackgroundColour{} == core::BackgroundColour{ 96, 128, 160 },
			"A default BackgroundColour is not (96, 128, 160)");
	}

	// A Background hosts nothing. Not even a Marker, which would be a goal no
	// agent could ever reach.
	void aBackgroundHostsNothing()
	{
		auto const background = makeBackground();

		for (auto const type : AllSectorObjectTypes)
		{
			require(!background.sectorSupportsObjectType(type),
				("A Background claims to host " + nameOf(type)).c_str());
		}
	}

	// A Window on the Layer in front may look into a Background. Nothing else may
	// use one as its far side.
	void aBackgroundIsLookTargetForAWindowOnly()
	{
		auto const background = makeBackground();

		for (auto const type : AllSectorObjectTypes)
		{
			auto const expected = type == core::SectorObjectType::Window;

			require(background.sectorSupportsObjectAsLookTarget(type) == expected,
				("A Background's look-target answer for " + nameOf(type)
					+ " is not " + (expected ? "true" : "false")).c_str());
		}
	}

	// The base class needs a top deck height; nothing walks a Background's decks,
	// so it is fixed at CORE_ROOM_MAX_HEIGHT and carries no capacity.
	void theDeckGeometryIsFixed()
	{
		auto background = makeBackground();

		require(background.getTopDeckHeight() == CORE_ROOM_MAX_HEIGHT,
			"A Background's top deck height is not CORE_ROOM_MAX_HEIGHT");
		require(background.getCapacity() == 0, "A Background reports a capacity");
		require(background.getCellsWide() == 5 && background.getDecksHigh() == 2,
			"A Background does not report its size");
		require(background.getCellX0() == 3 && background.getCellX1() == 7
			&& background.getCellY0() == 4 && background.getCellY1() == 5,
			"A Background does not cover the cells it was given");
		require(background.getSize().y == 1.0f + CORE_ROOM_MAX_HEIGHT,
			"A Background's height does not follow its decks");
	}

	// Nothing is walkable on a Background: the cells it owns carry no floor.
	void aBackgroundOwnsNoWalkableFloor()
	{
		core::CellDefinition cell;
		cell.floorType = core::Background::cellFloorType();

		require(core::Background::cellFloorType() == core::CellFloorType::None,
			"A Background's cell floor type is not CellFloorType::None");
		require(!cell.isTraversableOnFoot(),
			"A Background cell is traversable on foot");
	}

	// Dormancy: Building has no path to a Background, so a built Building holds
	// none. This stops down when #30 lands addBackground, deliberately.
	void noBuildingHoldsABackgroundYet()
	{
		core::Building building("Dormant", 8, 3);
		building.addRoom("Room 0", 0, 0, 0, 4, 1);
		building.addRoom("Room 1", 1, 0, 0, 4, 1);
		building.finishBuild();

		for (uint32_t layer = 0; layer < building.getLayerCount(); ++layer)
		{
			for (auto const& sector : building.getSectors(layer))
			{
				require(sector && sector->getType() != core::SectorType::Background,
					"A Building produced a Background before one can be authored");
			}
		}
	}
}

void runBackgroundSectorSmokeChecks()
{
	theTypeHasAString();
	aBackgroundIsItsOwnSectorKind();
	theColourDefaultsAndRoundTrips();
	aBackgroundHostsNothing();
	aBackgroundIsLookTargetForAWindowOnly();
	theDeckGeometryIsFixed();
	aBackgroundOwnsNoWalkableFloor();
	noBuildingHoldsABackgroundYet();
}
