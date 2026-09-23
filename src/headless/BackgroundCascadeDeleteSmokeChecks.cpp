// The thing behind a Window going away takes the Window with it, for ticket #33.
//
// A Window looks into a single Background (#32) and cannot look into nothing, so
// every edit which takes a Background away from a Window's back cells has to
// remove that Window too.  What the ticket adds is that the removal is announced
// rather than discovered: each affected Window is named in the plan's
// consequences, and the plan is what the editor confirms before anything is
// applied.
//
// Four trigger paths are pinned down here, each checked for the same two things -
// the plan names every Window it is about to delete and nothing else, and the
// applied edit deletes exactly those Windows:
//
//   delete the Background            -> every Window looking into it
//   move the Background away         -> every Window it uncovers
//   shrink the Background past them  -> only the Windows it lets go of
//   delete the Background's Layer    -> the Windows on the Layer in front
//
// Alongside them: planning changes nothing on its own, a Window looking into a
// real Location is left alone by every Background edit, and the cascade survives
// a serialisation round-trip with nothing dangling.

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Background.h"
#include "core/World.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Window.h"
#include "core/WindowSectorObject.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// Every distinct Window in the World, keyed by the Layer it is authored on
	// and the cell it sits on.  Sector indices shift under every edit, so the key
	// is what survives to be compared before and after.
	std::set<std::string> windowKeys(core::World const& world)
	{
		std::set<std::string> keys;
		for (uint32_t sector = 0; sector < world.getNumSectors(); ++sector)
		{
			auto const sectorPtr = world.getSector(sector);
			require(sectorPtr != nullptr, "World reported a null Sector while finding Windows");
			for (uint32_t object = 0; object < sectorPtr->getNumObjects(); ++object)
			{
				auto windowObject = std::dynamic_pointer_cast<const core::WindowSectorObject>(
					sectorPtr->getObject(object));
				if (!windowObject || !windowObject->getWindow()) continue;
				keys.insert(std::format("Layer {}.cell {},{}",
					windowObject->getWindow()->getFrontLayer(),
					windowObject->getCellX(), windowObject->getCellY()));
			}
		}
		return keys;
	}

	// Whether the consequence list names the Window at this cell on this Layer.
	// Both wordings the World uses - "looking into this Background" for a
	// Background edit and "which looks into <Layer>" for a Layer deletion - carry
	// the same "Window at x,y on Layer n" prefix.
	bool namesWindow(std::vector<std::string> const& consequences, uint32_t layer,
		uint32_t x, uint32_t y)
	{
		auto const needle = std::format("Window at {},{} on Layer {}", x, y, layer);
		return std::any_of(consequences.begin(), consequences.end(), [&](std::string const& line)
		{
			return line.find(needle) != std::string::npos;
		});
	}

	// The Windows the consequence list names, as keys.  Anything named which is
	// not one of the expected Windows is as much a failure as a Window left out.
	std::set<std::string> namedWindows(core::World const& world,
		std::vector<std::string> const& consequences)
	{
		std::set<std::string> named;
		for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
			for (uint32_t x = 0; x < world.getCellsWide(); ++x)
				for (uint32_t y = 0; y < world.getLevelsHigh(); ++y)
					if (namesWindow(consequences, layer, x, y))
						named.insert(std::format("Layer {}.cell {},{}", layer, x, y));
		return named;
	}

	// The whole ticket in one arrangement: one Room up front, two Backgrounds and
	// a real Room behind it, and a deep Layer to keep the World three Layers
	// wide no matter which of them goes.
	//
	//   Layer 0  Front Room                                  (Windows authored)
	//   Layer 1  Backdrop A (0-5) | Backdrop B (6-8) | Behind Room (9-11)
	//   Layer 2  Deep Room
	//
	// Four Windows look back from the Front Room, two into Backdrop A, one into
	// Backdrop B, and one into the Behind Room.  The last of those is the control
	// which must survive every Background edit, and Backdrop B is the one which
	// must survive every edit of Backdrop A.
	struct CascadeLayout
	{
		uint32_t front{ 0 };
		uint32_t backdropA{ 0 };
		uint32_t backdropB{ 0 };
		uint32_t behind{ 0 };
		uint32_t deep{ 0 };
	};

	CascadeLayout authorCascade(core::World& world)
	{
		CascadeLayout layout;
		while (world.getLayerCount() < 3) world.addLayer();
		layout.front = world.addRoom("Front", 0, 0, 0, 12, 1);
		layout.backdropA = world.addBackground(1, 0, 0, 6, 1, { 200, 120, 40 });
		layout.backdropB = world.addBackground(1, 0, 6, 3, 1, { 40, 160, 120 });
		layout.behind = world.addRoom("Behind", 1, 0, 9, 3, 1);
		layout.deep = world.addRoom("Deep", 2, 0, 0, 12, 1);
		return layout;
	}

	// The four Windows, in authoring order: two looking into Backdrop A, one into
	// Backdrop B, one into the Behind Room.
	void authorLookingWindows(core::World& world)
	{
		world.addSectorWindow(0, 0, 1, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		world.addSectorWindow(0, 0, 4, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		world.addSectorWindow(0, 0, 6, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		world.addSectorWindow(0, 0, 9, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
	}

	std::string keysToString(std::set<std::string> const& keys)
	{
		std::string text;
		for (auto const& key : keys)
		{
			if (!text.empty()) text += " | ";
			text += key;
		}
		return text.empty() ? "(none)" : text;
	}

	std::string consequencesToString(std::vector<std::string> const& consequences)
	{
		std::string text;
		for (auto const& line : consequences)
		{
			if (!text.empty()) text += " | ";
			text += line;
		}
		return text.empty() ? "(none)" : text;
	}

	// The edit deletes exactly the Windows the plan named: nothing more, nothing
	// less.  Every path in the ticket is checked this way.
	void requireExactlyAnnouncedAndDeleted(core::World& world,
		core::World::LocationEditPlan const& plan,
		std::set<std::string> const& expectedDeleted,
		std::set<std::string> const& before,
		std::string const& path)
	{
		require(plan.valid, (path + ": plan was rejected: " + plan.diagnostic).c_str());
		require(!plan.consequences.empty() == !expectedDeleted.empty(),
			(path + ": consequence list did not match the cascade: "
				+ consequencesToString(plan.consequences)).c_str());
		if (expectedDeleted.empty())
		{
			require(!plan.requiresConfirmation(),
				(path + ": an edit which deletes no Window still asked for confirmation").c_str());
			return;
		}

		require(plan.requiresConfirmation(),
			(path + ": a cascade which deletes Windows did not ask for confirmation").c_str());
		auto const named = namedWindows(world, plan.consequences);
		require(named == expectedDeleted,
			(path + ": the plan named " + keysToString(named)
				+ ", expected " + keysToString(expectedDeleted)).c_str());

		world.applyBackgroundEdit(plan);
		auto const after = windowKeys(world);
		for (auto const& key : expectedDeleted)
			require(before.count(key) != 0,
				(path + ": expected to delete " + key + ", which was never there").c_str());
		for (auto const& key : before)
		{
			bool const shouldSurvive = expectedDeleted.count(key) == 0;
			require(shouldSurvive ? after.count(key) != 0 : after.count(key) == 0,
				(path + ": Window " + key + (shouldSurvive ? " was deleted but should have survived"
					: " survived but should have been deleted"))
				.c_str());
		}
		require(after.size() == before.size() - expectedDeleted.size(),
			(path + ": Window count did not fall by the announced amount").c_str());
		require(world.isTraversalTopologyValid(),
			(path + ": the cascade left an invalid topology: "
				+ world.getTopologyDiagnostic()).c_str());
	}

	// Path 1: deleting a Background names every Window looking into it, and only
	// those, and the applied plan removes them with it.
	void deletingABackgroundTakesTheWindowsLookingIntoIt()
	{
		core::World world("Background delete", 12, 3);
		auto const layout = authorCascade(world);
		authorLookingWindows(world);
		world.finishBuild();

		auto const before = windowKeys(world);
		require(before.size() == 4,
			("The cascade map did not author four Windows: " + keysToString(before)).c_str());

		auto const plan = world.planRemoveBackground(layout.backdropA);
		// Planning is a question, not an edit: nothing has moved yet.
		require(windowKeys(world) == before,
			"Planning a Background delete changed the World");
		require(world.getSector(layout.backdropA) != nullptr
			&& world.getSector(layout.backdropA)->getType() == core::SectorType::Background,
			"Planning a Background delete removed the Background");

		requireExactlyAnnouncedAndDeleted(world, plan,
			{ "Layer 0.cell 1,0", "Layer 0.cell 4,0" }, before, "delete Background");

		// What is left reads the same from both sides: Backdrop B still stands, and
		// the Windows looking into it and into the Behind Room still look into them.
		require(world.getNumSectors() == 4,
			("Deleting one Background left the wrong number of Sectors: ")
			+ std::to_string(world.getNumSectors()));
		auto const left = windowKeys(world);
		require(left == std::set<std::string>{ "Layer 0.cell 6,0", "Layer 0.cell 9,0" },
			("The wrong Windows survived: " + keysToString(left)).c_str());
	}

	// A Background with nothing looking into it is an ordinary removal: no
	// consequences, and no confirmation asked for.
	void deletingABackgroundNobodyLooksIntoNeedsNoConfirmation()
	{
		core::World world("Unwatched Background", 12, 3);
		auto const layout = authorCascade(world);
		world.addSectorWindow(0, 0, 1, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		world.finishBuild();

		auto const plan = world.planRemoveBackground(layout.backdropB);
		require(plan.valid, ("Removing an unwatched Background was refused: " + plan.diagnostic).c_str());
		require(plan.consequences.empty(),
			("Removing a Background nobody looks into announced a cascade: "
				+ consequencesToString(plan.consequences)).c_str());
		require(!plan.requiresConfirmation(),
			"An empty cascade still asked for confirmation");

		auto const before = windowKeys(world);
		world.applyBackgroundEdit(plan);
		require(windowKeys(world) == before,
			"Removing an unwatched Background took a Window with it");
	}

	// Path 2: moving a Background off the cells it covered uncovers the Windows
	// looking into them.
	void movingABackgroundAwayTakesTheWindowsItUncovers()
	{
		core::World world("Background move", 12, 3);
		auto const layout = authorCascade(world);
		authorLookingWindows(world);
		world.finishBuild();

		auto const before = windowKeys(world);
		// The Background slides one level down, leaving every back cell it had.
		auto const plan = world.planResizeBackground(layout.backdropA, 0, 1, 6, 1);
		require(plan.move, "A Background moved to a new cell was not planned as a move");
		requireExactlyAnnouncedAndDeleted(world, plan,
			{ "Layer 0.cell 1,0", "Layer 0.cell 4,0" }, before, "move Background");
	}

	// Path 3: a resize which lets go of only some of the cells uncovers only the
	// Windows behind them.
	void shrinkingABackgroundTakesOnlyTheWindowsItLetsGoOf()
	{
		core::World world("Background shrink", 12, 3);
		auto const layout = authorCascade(world);
		authorLookingWindows(world);
		world.finishBuild();

		auto const before = windowKeys(world);
		// Backdrop A keeps cells 0-4 and lets go of cell 5, which the Window at
		// 4,0 straddles.  The Window at 1,0 is untouched.
		auto const plan = world.planResizeBackground(layout.backdropA, 0, 0, 5, 1);
		require(!plan.move, "A resize which changed the width was planned as a pure move");
		requireExactlyAnnouncedAndDeleted(world, plan,
			{ "Layer 0.cell 4,0" }, before, "shrink Background");
	}

	// The same edit with the opposite outcome: a Background which still covers
	// every back cell it covered before deletes nothing and says nothing.
	void aResizeWhichKeepsEveryBackCellCascadesNothing()
	{
		core::World world("Background kept", 12, 3);
		auto const layout = authorCascade(world);
		authorLookingWindows(world);
		world.finishBuild();

		auto const before = windowKeys(world);
		auto const plan = world.planResizeBackground(layout.backdropA, 0, 0, 6, 1);
		require(plan.valid, ("The unchanged resize was refused: " + plan.diagnostic).c_str());
		require(plan.consequences.empty(),
			("An edit which uncovers nothing announced one: "
				+ consequencesToString(plan.consequences)).c_str());
		require(!plan.requiresConfirmation(),
			"An edit which deletes nothing asked for confirmation");
		world.applyBackgroundEdit(plan);
		require(windowKeys(world) == before,
			"An unchanged resize changed the Windows");
	}

	// A Window looking into a real Location is not a Background casualty: the
	// control Window behind the Behind Room survives every edit of the
	// Backgrounds beside it.
	void aWindowLookingIntoALocationSurvivesEveryBackgroundEdit()
	{
		char const* control = "Layer 0.cell 9,0";

		// Three edits of two different Backgrounds, each on its own fresh copy of
		// the map: applying the first would take the next one's subject along with
		// it, and the check would prove nothing.
		struct BackgroundEdit
		{
			char const* name;
			bool editsBackdropA;
			bool remove;
			uint32_t x, y, cellsWide, levelsHigh;
			std::set<std::string> expectedDeleted;
		};

		std::vector<BackgroundEdit> edits{
			{ "delete Backdrop A", true, true, 0, 0, 0, 0,
				{ "Layer 0.cell 1,0", "Layer 0.cell 4,0" } },
			{ "move Backdrop B", false, false, 0, 1, 3, 1,
				{ "Layer 0.cell 6,0" } },
			{ "shrink Backdrop A", true, false, 0, 0, 1, 1,
				{ "Layer 0.cell 1,0", "Layer 0.cell 4,0" } }
		};

		for (auto const& edit : edits)
		{
			core::World world("Control check", 12, 3);
			auto const layout = authorCascade(world);
			authorLookingWindows(world);
			world.finishBuild();

			auto const before = windowKeys(world);
			require(before.count(control) != 0, "The control Window was not authored");

			auto const sectorIndex = edit.editsBackdropA ? layout.backdropA : layout.backdropB;
			auto const plan = edit.remove
				? world.planRemoveBackground(sectorIndex)
				: world.planResizeBackground(sectorIndex, edit.x, edit.y,
					edit.cellsWide, edit.levelsHigh);
			require(plan.valid, (std::string(edit.name) + ": refused: " + plan.diagnostic).c_str());
			require(!namesWindow(plan.consequences, 0, 9, 0),
				(std::string(edit.name) + ": named the Window looking into the Behind Room: "
					+ consequencesToString(plan.consequences)).c_str());
			requireExactlyAnnouncedAndDeleted(world, plan, edit.expectedDeleted, before, edit.name);
			require(windowKeys(world).count(control) != 0,
				(std::string(edit.name) + ": the control Window did not survive").c_str());
		}
	}

	// Path 4: deleting the Background's Layer takes the Windows on the Layer in
	// front which looked into it, and names each of them.
	void deletingTheBackgroundsLayerNamesTheWindowsLookingIntoIt()
	{
		core::World world("Layer delete", 12, 3);
		authorCascade(world);
		authorLookingWindows(world);
		world.finishBuild();

		auto const before = windowKeys(world);
		require(before.size() == 4, "The cascade map did not author four Windows");

		auto const plan = world.planDeleteLayer(1);
		require(plan.valid, ("Deleting the Background's Layer was refused: " + plan.diagnostic).c_str());
		require(plan.backgroundsRemoved == 2,
			("The Layer deletion did not report both Backgrounds: ")
			+ std::to_string(plan.backgroundsRemoved));

		// Every Window on the Layer in front looks into Layer 1, so every one of
		// them is named: the Background lookers and the Location looker alike.
		auto const named = namedWindows(world, plan.consequences);
		require(named == std::set<std::string>{ "Layer 0.cell 1,0", "Layer 0.cell 4,0",
			"Layer 0.cell 6,0", "Layer 0.cell 9,0" },
			("The Layer deletion named " + keysToString(named)
				+ ", expected all four looking Windows").c_str());

		world.applyDeleteLayer(plan);
		require(world.getLayerCount() == 2, "The Layers were not compacted");
		require(windowKeys(world).empty(),
			("A Window survived the deletion of what it looked into: "
				+ keysToString(windowKeys(world))).c_str());
		require(world.isTraversalTopologyValid(),
			("The Layer deletion left an invalid topology: "
				+ world.getTopologyDiagnostic()).c_str());
	}

	// The looking-in list belongs to the Layer which is going.  Deleting the Layer
	// in front takes the Windows as residents of that Layer, not as lookers, so
	// no "looks into" consequence is added.
	void deletingTheFrontLayerAddsNoLookingInConsequences()
	{
		core::World world("Front layer delete", 12, 3);
		authorCascade(world);
		authorLookingWindows(world);
		world.finishBuild();

		auto const plan = world.planDeleteLayer(0);
		require(plan.valid, ("Deleting the front Layer was refused: " + plan.diagnostic).c_str());
		for (auto const& consequence : plan.consequences)
			require(consequence.find("looks into") == std::string::npos,
				("Deleting the front Layer reported a looking-in casualty: " + consequence).c_str());
	}

	// The cascade is a rewrite of the authored records, so it has to survive the
	// round-trip: what was deleted stays deleted, and what survived is still
	// looking into what it looks into.
	void theCascadeSurvivesSerialisationReplay()
	{
		core::World world("Cascade replay", 12, 3);
		auto const layout = authorCascade(world);
		authorLookingWindows(world);
		world.finishBuild();

		auto const plan = world.planRemoveBackground(layout.backdropA);
		require(plan.valid, ("The cascade plan was refused: " + plan.diagnostic).c_str());
		world.applyBackgroundEdit(plan);

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();

		core::World reloaded("Cascade replay", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(reloaded.deserialize(*reader, workData),
			"The post-cascade World did not reload");

		auto const before = windowKeys(world);
		auto const after = windowKeys(reloaded);
		require(before == after,
			("The cascade did not round-trip: saved " + keysToString(before)
				+ ", reloaded " + keysToString(after)).c_str());
		require(after.size() == 2, "The reloaded World holds the wrong Windows");

		// And the survivor really is still looking into Backdrop B rather than
		// into a hole left by its neighbour.
		for (uint32_t sector = 0; sector < reloaded.getNumSectors(); ++sector)
		{
			auto const sectorPtr = reloaded.getSector(sector);
			for (uint32_t object = 0; object < sectorPtr->getNumObjects(); ++object)
			{
				auto windowObject = std::dynamic_pointer_cast<const core::WindowSectorObject>(
					sectorPtr->getObject(object));
				if (!windowObject || !windowObject->getWindow()) continue;
				require(windowObject->getWindow()->getBackSector() != nullptr,
					"A surviving Window reloaded with nothing behind it");
			}
		}
	}
}

void runBackgroundCascadeDeleteSmokeChecks()
{
	deletingABackgroundTakesTheWindowsLookingIntoIt();
	deletingABackgroundNobodyLooksIntoNeedsNoConfirmation();
	movingABackgroundAwayTakesTheWindowsItUncovers();
	shrinkingABackgroundTakesOnlyTheWindowsItLetsGoOf();
	aResizeWhichKeepsEveryBackCellCascadesNothing();
	aWindowLookingIntoALocationSurvivesEveryBackgroundEdit();
	deletingTheBackgroundsLayerNamesTheWindowsLookingIntoIt();
	deletingTheFrontLayerAddsNoLookingInConsequences();
	theCascadeSurvivesSerialisationReplay();
}
