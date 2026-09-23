// Threshold refusal audit, for ticket #31.
//
// The rule this pins down: no threshold of any kind may touch a Background,
// except a Window looking into it as the back side. The refusals fall out of
// two already-settled facts: a Background is not a Location, and its cells
// carry CellFloorType::None. Every guard trips on one or both - and every
// refusal must carry a message a user would understand, not a stack trace.
//
// Truth table covered here:
//   Door, Background behind            -> refused
//   Door, authored in a Background    -> refused
//   BulkheadDoor, Background on either side (same Layer) -> refused
//   Ladder, Background landing (either end)              -> refused
//   Ladder, Background inside its shaft column           -> refused
//   Room Ladder hosted by a Background                   -> refused
//   Stairwell, Background landing                        -> refused
//   Staircase, Background landing                        -> refused
//   Lift, Background landing                             -> refused
//   Shuttle, Background landings                         -> refused (all modes)
//   Shuttle partial landing facing a Background          -> door omitted, not built
//   Window looking into a Background                     -> ALLOWED (the exception)
//   Window authored in a Background                      -> refused
//   Traversable Window facing a Background               -> refused

#include <functional>
#include <stdexcept>
#include <string>

#include "core/Background.h"
#include "core/World.h"
#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorType.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	bool throws(std::function<void()> action)
	{
		try
		{
			action();
		}
		catch (std::exception const&)
		{
			return true;
		}
		return false;
	}

	// A refusal must carry a message a user would understand: non-empty, and
	// naming what went wrong rather than dumping internals.
	void requireUserMessage(std::string const& what, std::string const& message)
	{
		require(!message.empty(), (what + " was refused without a user-readable message").c_str());
		require(message.find("nullptr") == std::string::npos
			&& message.find("segmentation") == std::string::npos,
			(what + " carried a non-human message: " + message).c_str());
	}

	// A canAdd-style refusal: refused, with a diagnostic a user could read.
	void refusedQuery(std::string const& what, bool allowed, std::string const& diagnostic)
	{
		require(!allowed, (what + " was accepted against a Background").c_str());
		requireUserMessage(what, diagnostic);
	}

	// A throwing add path: refused, with the exception's message readable.
	void refusedThrow(std::string const& what, std::function<void()> action)
	{
		std::string message;
		bool threw = false;
		try
		{
			action();
		}
		catch (std::exception const& error)
		{
			threw = true;
			message = error.what();
		}
		require(threw, (what + " was accepted against a Background").c_str());
		requireUserMessage(what, message);
	}

	// A Door with a Background directly behind it: the back-side refusal.
	void doorRefusesABackgroundBehindIt()
	{
		core::World world("Door vs bg-behind", 12, 3);
		world.addLayer();
		world.addCorridor(0, 0, 0, 6, 1);
		world.addBackground(1, 0, 0, 6, 1);

		std::string diagnostic;
		refusedQuery("A Door with a Background behind it",
			world.canAddCorridorDoor(0, 0, 2, &diagnostic), diagnostic);
		refusedThrow("addSectorDoor with a Background behind it",
			[&] { world.addSectorDoor(0, 0, 2); });
	}

	// A Door that would be authored inside a Background, looking into a Room
	// behind: the front-side refusal.
	void doorRefusesToBeAuthoredInABackground()
	{
		core::World world("Door in bg", 12, 3);
		world.addLayer();
		world.addBackground(1, 0, 0, 6, 1);
		world.addRoom("Behind", 2, 0, 0, 6, 1);

		std::string diagnostic;
		refusedQuery("A Door authored in a Background",
			world.canAddCorridorDoor(1, 0, 2, &diagnostic), diagnostic);
		refusedThrow("addSectorDoor authored in a Background",
			[&] { world.addSectorDoor(1, 0, 2); });
	}

	// Same-layer BulkheadDoors are included: a Background is not a room, so
	// there is nothing to bulkhead. Test the shared edge from both sides.
	void bulkheadDoorRefusesABackgroundOnEitherSide()
	{
		core::World world("Bulkhead vs bg", 12, 3);
		world.addLayer();
		world.addBackground(1, 0, 0, 3, 1);
		world.addRoom("Room", 1, 0, 3, 4, 1);

		std::string diagnostic;
		refusedQuery("A BulkheadDoor with a Background on its left",
			world.canAddSectorBulkheadDoor(1, 0, 3, CORE_SIDE_LEFT, {}, &diagnostic), diagnostic);
		refusedQuery("A BulkheadDoor with a Background on its right",
			world.canAddSectorBulkheadDoor(1, 0, 2, CORE_SIDE_RIGHT, {}, &diagnostic), diagnostic);
		refusedThrow("addSectorBulkheadDoor against a Background",
			[&] { world.addSectorBulkheadDoor(1, 0, 3, CORE_SIDE_LEFT); });
	}

	// A Ladder may not land on a Background at either end of its span.
	void ladderRefusesABackgroundLanding()
	{
		{
			core::World world("Ladder lower=bg", 12, 3);
			world.addLayer();
			world.addBackground(1, 0, 0, 2, 1);
			world.addRoom("Upper", 1, 1, 0, 4, 1);

			std::string diagnostic;
			refusedQuery("A Ladder landing on a Background below",
				world.canAddLadder(2, 0, 0, 2, &diagnostic), diagnostic);
			refusedThrow("addLadder landing on a Background below",
				[&] { world.addLadder(2, 0, 0, { 2, false, true }); });
		}
		{
			core::World world("Ladder upper=bg", 12, 3);
			world.addLayer();
			world.addRoom("Lower", 1, 0, 0, 4, 1);
			world.addBackground(1, 1, 0, 2, 1);

			std::string diagnostic;
			refusedQuery("A Ladder landing on a Background above",
				world.canAddLadder(2, 0, 0, 2, &diagnostic), diagnostic);
			refusedThrow("addLadder landing on a Background above",
				[&] { world.addLadder(2, 0, 0, { 2, false, true }); });
		}
	}

	// A Background inside the Ladder's own shaft column blocks it like any
	// other Sector on that Layer.
	void ladderRefusesABackgroundInItsShaft()
	{
		core::World world("Ladder shaft=bg", 12, 3);
		world.addLayer();
		world.addRoom("Lower", 1, 0, 0, 4, 1);
		world.addRoom("Upper", 1, 1, 0, 4, 1);
		world.addBackground(2, 0, 0, 4, 2);

		std::string diagnostic;
		refusedQuery("A Ladder whose shaft crosses a Background",
			world.canAddLadder(2, 0, 0, 2, &diagnostic), diagnostic);
	}

	// A Room Ladder cannot be hosted by a Background either.
	void roomLadderRefusesABackgroundHost()
	{
		core::World world("Room ladder on bg", 12, 3);
		world.addLayer();
		auto const backdrop = world.addBackground(1, 0, 0, 4, 2);

		std::string diagnostic;
		refusedQuery("A Room Ladder hosted by a Background",
			world.canAddRoomLadder(backdrop, 0, 0, nullptr, &diagnostic), diagnostic);
		refusedThrow("addRoomLadder hosted by a Background",
			[&] { world.addRoomLadder(backdrop, 0, 0); });
	}

	// A Stairwell may not land on a Background.
	void stairwellRefusesABackgroundLanding()
	{
		core::World world("Stairwell vs bg", 12, 3);
		world.addLayer();
		world.addBackground(1, 0, 0, 2, 2);

		std::string diagnostic;
		refusedQuery("A Stairwell landing on a Background",
			world.canAddStairwell(2, 0, 0, 2, &diagnostic), diagnostic);
		refusedThrow("addStairwell landing on a Background",
			[&] { world.addStairwell(2, 0, 0, { 2, CORE_SIDE_LEFT }); });
	}

	// A Staircase may not land on a Background at either endpoint.
	void staircaseRefusesABackgroundLanding()
	{
		core::World world("Staircase vs bg", 12, 3);
		world.addLayer();
		world.addBackground(1, 0, 0, 4, 2);

		std::string diagnostic;
		refusedQuery("A Staircase landing on a Background",
			world.canAddStaircase(2, 0, 0, 2, CORE_SIDE_RIGHT, &diagnostic), diagnostic);
		refusedThrow("addStaircase landing on a Background",
			[&] { world.addStaircase(2, 0, 0, { 2, CORE_SIDE_RIGHT, 0.0f }); });
	}

	// An enclosed Lift may not stop at a Background.
	void liftRefusesABackgroundLanding()
	{
		core::World world("Lift vs bg", 12, 3);
		world.addLayer();
		world.addBackground(1, 0, 0, 4, 2);

		refusedThrow("addLift stopping at a Background",
			[&] { world.addLift(2, 0, 0, { 1, { 0, 1 } }); });
	}

	// A Shuttle refuses a Background landing in every mode: strict refuses
	// outright, and partial mode refuses when a stop has no Location landing
	// at all.
	void shuttleRefusesABackgroundLanding()
	{
		{
			core::World world("Shuttle all bg", 12, 3);
			world.addLayer();
			world.addBackground(1, 0, 0, 8, 1);

			refusedThrow("addShuttle landing on a Background",
				[&] { world.addShuttle(2, 0, 0, 8, { 1, 3, { 0, 4 }, 0 }); });
			refusedThrow("addShuttle landing on a Background, partial mode",
				[&]
				{
					core::World::CreateShuttleOptions options{ 1, 3, { 0, 4 }, 0 };
					options.allowPartialLandings = true;
					world.addShuttle(2, 0, 0, 8, options);
				});
		}
		{
			// Partial mode beside a Room: the shuttle is accepted, but the
			// carriage door that would have faced the Background is omitted -
			// no threshold is built that touches the Background.
			core::World world("Shuttle mixed landing", 12, 3);
			world.addLayer();
			world.addBackground(1, 0, 0, 2, 1);
			world.addRoom("Stop room", 1, 0, 2, 10, 1);

			core::World::CreateShuttleOptions options{ 1, 3, { 0, 4 }, 0 };
			options.allowPartialLandings = true;
			// Doors at carriage cells 0 and 2: at stop 0 those sit over the
			// Background and the Room respectively.
			options.doorMask = (1u << 0) | (1u << 2);
			auto result = world.addShuttle(2, 0, 0, 8, options);

			require(result.doors.size() == 4, "Shuttle door result grid changed shape");
			require(result.doors[0].door.sector == nullptr,
				"A partial Shuttle built a carriage door touching a Background");
			require(!result.doors[0].traversalResource,
				"A partial Shuttle gave a Background-facing door a traversal resource");
			require(result.doors[1].door.sector != nullptr,
				"A partial Shuttle omitted a door that lands on a Room");
			require(result.doors[2].door.sector != nullptr
				&& result.doors[3].door.sector != nullptr,
				"A partial Shuttle omitted stop doors that land on a Room");
		}
	}

	// The one exception: a Window on the Layer in front may look into a
	// Background as its back side.
	void windowMayLookIntoABackground()
	{
		core::World world("Window looks into bg", 12, 3);
		world.addLayer();
		world.addRoom("Front", 0, 0, 0, 6, 1);
		world.addBackground(1, 0, 0, 6, 1);

		std::string diagnostic;
		require(world.canAddSectorWindow(0, 0, 1, 2, 1, &diagnostic),
			("A Window was refused looking into a Background: " + diagnostic).c_str());
		require(!throws([&]
			{
				world.addSectorWindow(0, 0, 1, 2, 1,
					{ false, core::Window::State::Closed, core::Window::Style::Clear });
			}),
			"addSectorWindow refused to look into a Background");
	}

	// The exception does not stretch: a Window may not be authored in a
	// Background, and a traversable Window may not cross into one.
	void windowExceptionStaysLookingOnly()
	{
		{
			core::World world("Window in bg", 12, 3);
			world.addLayer();
			world.addBackground(1, 0, 0, 6, 1);
			world.addRoom("Behind", 2, 0, 0, 6, 1);

			std::string diagnostic;
			refusedQuery("A Window authored in a Background",
				world.canAddSectorWindow(1, 0, 1, 2, 1, &diagnostic), diagnostic);
			refusedThrow("addSectorWindow authored in a Background",
				[&] { world.addSectorWindow(1, 0, 1, 2, 1); });
		}
		{
			core::World world("Traversable window vs bg", 12, 3);
			world.addLayer();
			world.addRoom("Front", 0, 0, 0, 6, 1);
			world.addBackground(1, 0, 0, 6, 1);

			refusedThrow("A traversable Window facing a Background",
				[&]
				{
					world.addSectorWindow(0, 0, 1, 2, 1,
						{ true, core::Window::State::Open, core::Window::Style::Clear });
				});
			// The same placement without traversal is still fine: the refusal
			// is about crossing, not looking.
			require(!throws([&]
				{
					world.addSectorWindow(0, 0, 1, 2, 1,
						{ false, core::Window::State::Closed, core::Window::Style::Clear });
				}),
				"A looking Window was refused after the traversable refusal");
		}
	}
}

void runThresholdRefusalSmokeChecks()
{
	doorRefusesABackgroundBehindIt();
	doorRefusesToBeAuthoredInABackground();
	bulkheadDoorRefusesABackgroundOnEitherSide();
	ladderRefusesABackgroundLanding();
	ladderRefusesABackgroundInItsShaft();
	roomLadderRefusesABackgroundHost();
	stairwellRefusesABackgroundLanding();
	staircaseRefusesABackgroundLanding();
	liftRefusesABackgroundLanding();
	shuttleRefusesABackgroundLanding();
	windowMayLookIntoABackground();
	windowExceptionStaysLookingOnly();
}
