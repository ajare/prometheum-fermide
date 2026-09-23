// A Window may span several Backgrounds behind it; a mixed span is refused, #36.
//
// The one-Sector rule exists because crossing a Sector boundary breaks the
// traversal geometry a caller is about to build. Behind a Window there is no such
// geometry: a Background is seen, never entered, so a wide Window looking across
// the seam between two Backgrounds still sees an unbroken view. Half room-interior
// and half sky is a different matter - there should be a wall where the room ends -
// so a span that mixes a Location with a Background is refused outright.
//
// Window keeps a single back Sector. For a multi-Background span it stays the
// first Background in the span and is treated as non-authoritative: such Windows
// are never traversable (a Background may not be entered), so nothing reads the
// field for traversal. The renderer derives truth from the cell grid (#37).
//
//   Backgrounds side by side behind a Window   -> accepted, however many it spans
//   Back Sector of that Window                 -> first Background, not a promise
//   Background + Location behind one Window    -> refused, and says why
//   Traversable Window over Backgrounds        -> still refused (#31 guard)
//   Every other threshold, every other Layer   -> one-Sector rule unchanged

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Background.h"
#include "core/World.h"
#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Window.h"
#include "core/WindowSectorObject.h"
#include "core/YamlSerializer.h"

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

	std::string exceptionMessage(std::function<void()> action)
	{
		try
		{
			action();
		}
		catch (std::exception const& error)
		{
			return error.what();
		}
		return {};
	}

	std::shared_ptr<const core::Sector> sectorByIndex(core::World const& world, uint32_t index)
	{
		auto sector = world.getSector(index);
		require(sector != nullptr, "World reported a null Sector");
		return sector;
	}

	uint32_t windowTraversalResources(core::World const& world)
	{
		uint32_t count{ 0 };
		for (auto const& resource : world.getSimulationSnapshot().traversalResources)
			if (resource.isWindow) ++count;
		return count;
	}

	std::string serializeWorld(core::World const& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	void loadInto(core::World& target, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(target.deserialize(*reader, workData), "World YAML did not load");
	}

	// The arrangement every span test starts from: one Room up front, Backgrounds
	// side by side behind it, and a Room on the Layer behind that.
	//
	//   Layer 0  Front Room                 (Windows are authored here)
	//   Layer 1  Sky Left | Sky Mid | Sky Right   (separate Backgrounds, never merged)
	//   Layer 2  Deep Room
	//
	// A Window from x 2 to x 9 looks across both seams, over all three Backgrounds.
	struct SkyLayout
	{
		uint32_t front{ 0 };
		uint32_t skyLeft{ 0 };
		uint32_t skyMid{ 0 };
		uint32_t skyRight{ 0 };
	};

	SkyLayout authorBackgroundRow(core::World& world)
	{
		SkyLayout layout;
		while (world.getLayerCount() < 3) world.addLayer();
		layout.front = world.addRoom("Front", 0, 0, 0, 12, 1);
		layout.skyLeft = world.addBackground(1, 0, 0, 4, 1, { 200, 120, 40 });
		layout.skyMid = world.addBackground(1, 0, 4, 4, 1, { 30, 90, 150 });
		layout.skyRight = world.addBackground(1, 0, 8, 4, 1, { 90, 160, 90 });
		world.addRoom("Deep", 2, 0, 0, 12, 1);
		return layout;
	}

	// The same shape with a Room behind half of it, so a Window can straddle the
	// seam between sky and room interior in either direction.
	struct MixedLayout
	{
		uint32_t front{ 0 };
		uint32_t sky{ 0 };
		uint32_t behind{ 0 };
	};

	MixedLayout authorBackgroundAndRoom(core::World& world)
	{
		MixedLayout layout;
		while (world.getLayerCount() < 3) world.addLayer();
		layout.front = world.addRoom("Front", 0, 0, 0, 12, 1);
		layout.sky = world.addBackground(1, 0, 0, 6, 1, { 200, 120, 40 });
		layout.behind = world.addRoom("Behind", 1, 0, 6, 6, 1);
		return layout;
	}

	// Each Background keeps its own cells after a Window spans them: the span is
	// tolerated, not merged.
	void backgroundsStaySeparate(core::World const& world, SkyLayout const& layout)
	{
		auto const layer = world.getLayer(1);
		for (uint32_t x = 0; x < 4; ++x)
			require(layer->getCellDefinition(x, 0).sectorIndex == layout.skyLeft,
				std::format("Sky Left no longer owns cell {},0 on Layer 1", x).c_str());
		for (uint32_t x = 4; x < 8; ++x)
			require(layer->getCellDefinition(x, 0).sectorIndex == layout.skyMid,
				std::format("Sky Mid no longer owns cell {},0 on Layer 1", x).c_str());
		for (uint32_t x = 8; x < 12; ++x)
			require(layer->getCellDefinition(x, 0).sectorIndex == layout.skyRight,
				std::format("Sky Right no longer owns cell {},0 on Layer 1", x).c_str());
	}
}

// Two Backgrounds side by side behind one wide Window: accepted, with no diagnostic.
void aWindowSpanningSeveralBackgroundsIsAccepted()
{
	core::World world("Wide Window, two Backgrounds", 12, 3);
	auto const layout = authorBackgroundRow(world);

	// The seam the Window is asked to straddle really is a Sector boundary.
	require(sectorByIndex(world, layout.skyLeft)->getCellX1() == 3
		&& sectorByIndex(world, layout.skyMid)->getCellX0() == 4,
		"The two Backgrounds do not meet at the expected seam");

	std::string diagnostic;
	require(world.canAddSectorWindow(0, 0, 2, 6, 1, &diagnostic),
		("A Window spanning two Backgrounds was refused: " + diagnostic).c_str());
	require(diagnostic.empty(),
		("An accepted Window still carried a diagnostic: " + diagnostic).c_str());

	// The seam itself, dead centre on the boundary between two Backgrounds.
	require(world.canAddSectorWindow(0, 0, 2, 4, 1, &diagnostic),
		("A Window straddling the Background seam was refused: " + diagnostic).c_str());

	// Spanning three Backgrounds is no different from spanning two.
	require(world.canAddSectorWindow(0, 0, 1, 10, 1, &diagnostic),
		("A Window spanning three Backgrounds was refused: " + diagnostic).c_str());
}

// The Window's single back Sector is the first Background in the span, and is
// non-authoritative: it is a leftover of a narrower data model, not a promise
// about what lies behind the whole aperture.
void theBackSectorIsTheFirstBackgroundInTheSpan()
{
	core::World world("First Background wins", 12, 3);
	auto const layout = authorBackgroundRow(world);

	auto created = world.addSectorWindow(0, 0, 2, 6, 1,
		{ false, core::Window::State::Closed, core::Window::Style::Clear });
	world.finishBuild();

	require(created.object != nullptr, "The spanning Window has no Window");
	auto const back = created.object->getBackSector();
	require(back != nullptr, "The spanning Window has no back Sector");
	require(back->getType() == core::SectorType::Background,
		"The spanning Window's back Sector is not a Background");
	require(back->getIndex() == layout.skyLeft,
		std::format("The spanning Window's back Sector is not the first Background in the span (got {})",
			back->getIndex()).c_str());
	require(!created.object->isTraversalConfigured(),
		"A Window spanning Backgrounds reports its traversal as configured");
	require(!created.traversalResource,
		"A Window spanning Backgrounds was given a traversal resource");
	require(windowTraversalResources(world) == 0,
		"A Window spanning Backgrounds minted a traversal resource");

	backgroundsStaySeparate(world, layout);
}

// A span that mixes a Background with a Location is refused, in both orders, and
// the refusal says why rather than just reporting a multi-Sector crossing.
void aMixedBackgroundAndLocationSpanIsRefused()
{
	core::World world("Half room, half sky", 12, 3);
	auto const layout = authorBackgroundAndRoom(world);

	// Background first, Room behind the rest of the Window.
	std::string diagnostic;
	require(!world.canAddSectorWindow(0, 0, 4, 4, 1, &diagnostic),
		"A Window spanning a Background and a Room was accepted");
	require(diagnostic.find("Background") != std::string::npos,
		("The mixed-span refusal does not mention the Background: " + diagnostic).c_str());
	require(diagnostic.find("wall") != std::string::npos,
		("The mixed-span refusal does not say why: " + diagnostic).c_str());

	// Room first, Background behind the rest of the Window.
	require(!world.canAddSectorWindow(0, 0, 2, 6, 1, &diagnostic),
		"A Window spanning a Room and a Background was accepted");
	require(diagnostic.find("wall") != std::string::npos,
		("The mixed-span refusal does not say why: " + diagnostic).c_str());

	// The refusal is a refusal to author, not a half-built Window.
	require(throws([&]
		{
			world.addSectorWindow(0, 0, 4, 4, 1,
				{ false, core::Window::State::Closed, core::Window::Style::Clear });
		}),
		"addSectorWindow() accepted a mixed Background/Location span");
	require(windowTraversalResources(world) == 0,
		"The refused mixed span left a traversal resource behind");

	// The same Window entirely over the Background half, and entirely over the
	// Room half, are both still fine: the refusal is about the span, not the spot.
	require(world.canAddSectorWindow(0, 0, 1, 4, 1, &diagnostic),
		("A Window wholly over the Background was refused: " + diagnostic).c_str());
	require(world.canAddSectorWindow(0, 0, 7, 4, 1, &diagnostic),
		("A Window wholly over the Room was refused: " + diagnostic).c_str());

	// And the Background the mixed span touched is untouched by the refusal.
	require(sectorByIndex(world, layout.sky)->getType() == core::SectorType::Background,
		"The Background changed after a mixed span was refused");
	require(sectorByIndex(world, layout.behind)->getType() != core::SectorType::Background,
		"The Room behind the mixed span became a Background");
}

// A traversable Window over Backgrounds is still refused by the #31 guard: the
// relaxation is about looking, never about entering.
void aTraversableWindowOverTwoBackgroundsIsStillRefused()
{
	core::World world("Traversable over sky", 12, 3);
	authorBackgroundRow(world);

	require(windowTraversalResources(world) == 0,
		"The World starts with a traversal resource already");

	auto const message = exceptionMessage([&]
		{
			world.addSectorWindow(0, 0, 2, 6, 1,
				{ true, core::Window::State::Open, core::Window::Style::Clear });
		});
	require(!message.empty(), "A traversable Window over Backgrounds was accepted");
	require(message.find("Background") != std::string::npos,
		("The traversable refusal does not mention the Background: " + message).c_str());
	require(windowTraversalResources(world) == 0,
		"A refused traversable Window minted a traversal resource");
}

// The relaxation belongs to the Window's back Layer and nowhere else: the same
// multi-Sector span is still refused for a Door's back Layer, for a Window's own
// front Layer, and for a Ladder crossing Sectors.
void theOneSectorRuleHoldsEverywhereElse()
{
	// A Door's back Layer over two Backgrounds: refused, with the plain
	// multi-Sector message rather than the Window's mixed-span wording.
	{
		core::World world("Door over two Backgrounds", 12, 3);
		while (world.getLayerCount() < 3) world.addLayer();
		world.addCorridor(0, 0, 0, 12, 1);
		world.addBackground(1, 0, 0, 6, 1, { 200, 120, 40 });
		world.addBackground(1, 0, 6, 6, 1, { 30, 90, 150 });

		std::string diagnostic;
		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 6;
		require(!world.canAddCorridorDoor(0, 0, 2, doorOptions, &diagnostic),
			"A Door spanning two Backgrounds was accepted");
		require(diagnostic.find("cross multiple Sectors") != std::string::npos,
			("The Door refusal changed shape: " + diagnostic).c_str());
		require(diagnostic.find("wall") == std::string::npos,
			("The Window's mixed-span wording leaked into the Door refusal: " + diagnostic).c_str());
	}

	// A Door's back Layer over two Rooms: refused exactly as it always was.
	{
		core::World world("Door over two Rooms", 12, 3);
		while (world.getLayerCount() < 3) world.addLayer();
		world.addCorridor(0, 0, 0, 12, 1);
		world.addRoom("Left", 1, 0, 0, 6, 1);
		world.addRoom("Right", 1, 0, 6, 6, 1);

		std::string diagnostic;
		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 6;
		require(!world.canAddCorridorDoor(0, 0, 2, doorOptions, &diagnostic),
			"A Door spanning two Rooms was accepted");
		require(diagnostic.find("cross multiple Sectors") != std::string::npos,
			("The Door refusal changed shape: " + diagnostic).c_str());
	}

	// A Window's own front Layer keeps the strict rule: only the Layer it looks
	// into may span more than one Sector.
	{
		core::World world("Wide front span", 12, 3);
		while (world.getLayerCount() < 3) world.addLayer();
		world.addRoom("Left", 0, 0, 0, 6, 1);
		world.addRoom("Right", 0, 0, 6, 6, 1);
		world.addBackground(1, 0, 0, 12, 1, { 200, 120, 40 });

		std::string diagnostic;
		require(!world.canAddSectorWindow(0, 0, 2, 6, 1, &diagnostic),
			"A Window whose front Layer spans two Rooms was accepted");
		require(diagnostic.find("cross multiple Sectors") != std::string::npos,
			("The front-Layer refusal changed shape: " + diagnostic).c_str());
	}

	// A Ladder crossing Sectors on its own Layer is refused as it always was.
	{
		core::World world("Ladder across rooms", 12, 4);
		while (world.getLayerCount() < 3) world.addLayer();
		auto const lower = world.addRoom("Lower", 1, 0, 0, 6, 2);
		world.addRoom("Upper", 1, 2, 0, 6, 2);
		world.addRoom("Fore", 0, 0, 0, 6, 4);

		require(throws([&]
			{
				world.addSectorLadder(lower, 0, 1, { 4, false, true });
			}),
			"A Ladder crossing two Sectors was accepted");
	}
}

// A multi-Background span survives the writer/reader round trip: the relaxed rule
// is a placement rule, and replaying it lands the same Window in the same place,
// still spanning the same two Backgrounds.
void aMultiBackgroundSpanReplaysFromItsOwnRecords()
{
	core::World world("Span replay", 12, 3);
	auto const layout = authorBackgroundRow(world);
	world.addSectorWindow(0, 0, 2, 6, 1,
		{ false, core::Window::State::Closed, core::Window::Style::Clear });
	world.finishBuild();

	core::World reloaded("Span replay reload", 12, 3);
	loadInto(reloaded, serializeWorld(world));

	require(reloaded.getNumSectors() == world.getNumSectors(),
		"The replayed World has a different Sector count");
	auto const skyLeft = sectorByIndex(reloaded, layout.skyLeft);
	require(skyLeft->getType() == core::SectorType::Background,
		"The replayed left Background is not a Background");
	require(std::dynamic_pointer_cast<const core::Background>(skyLeft)->getColour()
		== core::BackgroundColour{ 200, 120, 40 },
		"The replayed left Background lost its colour");
	backgroundsStaySeparate(reloaded, layout);

	// The replayed Window is still the wide one, and its back Sector is still the
	// first Background in the span rather than anything the span also covers.
	std::shared_ptr<const core::Window> replayed;
	uint32_t replayedCellX{ ~0u };
	uint32_t distinctWindows{ 0 };
	for (uint32_t sectorIndex = 0; sectorIndex < reloaded.getNumSectors(); ++sectorIndex)
	{
		auto const sector = sectorByIndex(reloaded, sectorIndex);
		for (uint32_t objectIndex = 0; objectIndex < sector->getNumObjects(); ++objectIndex)
		{
			auto windowObject = std::dynamic_pointer_cast<const core::WindowSectorObject>(
				sector->getObject(objectIndex));
			if (!windowObject) continue;
			if (!replayed || windowObject->getWindow() != replayed) ++distinctWindows;
			if (!replayed)
			{
				replayed = windowObject->getWindow();
				replayedCellX = windowObject->getCellX();
			}
		}
	}
	require(replayed != nullptr, "The replayed World holds no Window");
	require(distinctWindows == 1,
		std::format("The replayed World holds {} distinct Windows, expected 1", distinctWindows).c_str());
	require(replayedCellX == 2 && replayed->getCellsWide() == 6,
		"The replayed Window is not the wide Window that was saved");
	require(replayed->getBackSector() != nullptr
		&& replayed->getBackSector()->getIndex() == layout.skyLeft,
		"The replayed Window's back Sector is not the first Background in its span");
	require(windowTraversalResources(reloaded) == 0,
		"The replayed span minted a traversal resource");
}

void runWindowMultiBackgroundSmokeChecks()
{
	aWindowSpanningSeveralBackgroundsIsAccepted();
	theBackSectorIsTheFirstBackgroundInTheSpan();
	aMixedBackgroundAndLocationSpanIsRefused();
	aTraversableWindowOverTwoBackgroundsIsStillRefused();
	theOneSectorRuleHoldsEverywhereElse();
	aMultiBackgroundSpanReplaysFromItsOwnRecords();
}
