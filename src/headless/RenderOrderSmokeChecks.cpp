// Tickets #16, #25 and #26: rendering order for multi-layer Worlds.
//
// The viewport paints the selected Layer solid and whole. The Layer directly
// behind it is painted by two passes: solid, clipped to the apertures the
// selected Layer gives it, and - while the wireframe overlay is on - outlined
// over the selection. Every other Layer is hidden.
//
// This scenario authors a four-Layer depot carrying one Transit of every clipped
// type at a different depth, then asks - for each Layer a viewer can select -
// exactly which world-space area of every Sector reaches the screen. Two
// properties are required of that painted area:
//
//   * a Transit one Layer behind the selection is painted solid where the
//     selected Layer's Locations open onto it, whether or not the overlay is on,
//     so it does not disappear inside a Room;
//   * it is painted solid nowhere else: not over the selected Layer's own floor,
//     not over ground the selected Layer leaves empty, and not at all from a Layer
//     which is neither the selection nor the one directly behind it.
//
// The overlay is the x-ray half of the picture. It outlines the whole Layer
// behind over the selection - including the ground its solid pass could not reach
// - but never fills a Sector, never draws a Transit's own geometry, and never
// shows the Agents standing inside it.
//
// The paint model below emits the same passes, in the same order, as
// renderWorld() does, and drives them through the very policy helpers the
// renderer calls - renderPasses, shouldClipTransitToApertures, transitApertures,
// shouldRenderSectorAgents, shouldFillBackground and shouldOutlineBackground -
// so a change to that policy shows up here rather than only on a screen.
//
// The Background checks (#35) ride the same model with a two-Layer backdrop of
// their own: a Background fills whenever its Layer is drawn solid, is outlined by
// the wireframe overlay alone, and meets its same-colour neighbour with no border
// drawn across the seam. The multi-Background aperture checks (#37) read the
// very backgroundApertureRegions() the renderer composites from, asserting each
// region's fill colour and clip bounds against the back Layer's cell grid.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Render.h"
#include "SectorTileset.h"
#include "core/Background.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Location.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/SectorType.h"
#include "core/Transit.h"
#include "core/Vector2.h"
#include "core/Window.h"
#include "core/WindowSectorObject.h"

namespace
{
	constexpr double kAreaEpsilon = 0.0001;

	void require(bool condition, char const* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	//
	// Axis-aligned world-space rectangle. Every Sector and aperture is one, so a
	// Sector's painted area is an exact union of rectangles rather than an
	// approximation of one.
	//
	struct Rect
	{
		double minX{ 0.0 };
		double minY{ 0.0 };
		double maxX{ 0.0 };
		double maxY{ 0.0 };

		double area() const
		{
			auto const width = maxX - minX;
			auto const height = maxY - minY;
			return width > 0.0 && height > 0.0 ? width * height : 0.0;
		}
	};

	Rect rectOf(core::Sector const& sector)
	{
		core::Vector2 lo, hi;
		sector.getBounds(lo, hi);
		return { static_cast<double>(lo.x), static_cast<double>(lo.y),
			static_cast<double>(hi.x), static_cast<double>(hi.y) };
	}

	Rect rectOf(TransitAperture const& aperture)
	{
		return { static_cast<double>(aperture.min.x), static_cast<double>(aperture.min.y),
			static_cast<double>(aperture.max.x), static_cast<double>(aperture.max.y) };
	}

	Rect rectOf(BackgroundApertureRegion const& region)
	{
		return { static_cast<double>(region.min.x), static_cast<double>(region.min.y),
			static_cast<double>(region.max.x), static_cast<double>(region.max.y) };
	}

	Rect rectOf(core::Vector2 const& lo, core::Vector2 const& hi)
	{
		return { static_cast<double>(lo.x), static_cast<double>(lo.y),
			static_cast<double>(hi.x), static_cast<double>(hi.y) };
	}

	Rect intersect(Rect const& a, Rect const& b)
	{
		Rect out{ std::max(a.minX, b.minX), std::max(a.minY, b.minY),
			std::min(a.maxX, b.maxX), std::min(a.maxY, b.maxY) };
		if (out.maxX - out.minX <= 0.0 || out.maxY - out.minY <= 0.0)
		{
			return {};
		}
		return out;
	}

	bool overlaps(Rect const& a, Rect const& b)
	{
		return intersect(a, b).area() > kAreaEpsilon;
	}

	// Exact union area: the rectangles' own edges cut the plane into cells, and a
	// cell counts once however many rectangles cover it.
	double unionArea(std::vector<Rect> const& rects)
	{
		std::vector<double> xs, ys;
		for (auto const& rect : rects)
		{
			if (rect.area() <= 0.0) continue;
			xs.push_back(rect.minX);
			xs.push_back(rect.maxX);
			ys.push_back(rect.minY);
			ys.push_back(rect.maxY);
		}
		std::sort(xs.begin(), xs.end());
		xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
		std::sort(ys.begin(), ys.end());
		ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

		double total{ 0.0 };
		for (size_t i = 0; i + 1 < xs.size(); ++i)
		{
			for (size_t j = 0; j + 1 < ys.size(); ++j)
			{
				Rect const cell{ xs[i], ys[j], xs[i + 1], ys[j + 1] };
				for (auto const& rect : rects)
				{
					if (rect.minX <= cell.minX + kAreaEpsilon && rect.maxX >= cell.maxX - kAreaEpsilon
						&& rect.minY <= cell.minY + kAreaEpsilon && rect.maxY >= cell.maxY - kAreaEpsilon)
					{
						total += cell.area();
						break;
					}
				}
			}
		}
		return total;
	}

	double unionAreaClippedTo(std::vector<Rect> const& rects, std::vector<Rect> const& clip)
	{
		std::vector<Rect> clipped;
		for (auto const& rect : rects)
			for (auto const& c : clip)
			{
				auto const piece = intersect(rect, c);
				if (piece.area() > 0.0) clipped.push_back(piece);
			}
		return unionArea(clipped);
	}

	std::string describe(double value)
	{
		std::ostringstream stream;
		stream.setf(std::ios::fixed);
		stream.precision(4);
		stream << value;
		return stream.str();
	}

	std::string describeRect(Rect const& rect)
	{
		std::ostringstream stream;
		stream.setf(std::ios::fixed);
		stream.precision(2);
		stream << "(" << rect.minX << ", " << rect.minY << ")-(" << rect.maxX << ", " << rect.maxY << ")";
		return stream.str();
	}

	//
	// What one pass paints of one Sector: a rectangle, either filled or outlined.
	//
	struct PaintStroke
	{
		Rect rect;
		bool solid{ true };
	};

	//
	// What one Sector contributes to the screen while a given Layer is selected,
	// gathered from every pass which touches it and kept in draw order.
	//
	struct PaintedSector
	{
		uint32_t index{ 0 };
		std::string name;
		core::SectorType sectorType{ core::SectorType::Location };
		uint32_t layer{ 0 };
		bool drawn{ false };
		// Agents are painted by a pass which draws its Sector solid. The overlay
		// never paints them, so this says "some pass was allowed to show them".
		bool agentsVisible{ false };
		Rect footprint;
		std::vector<PaintStroke> strokes;

		std::vector<Rect> rectsOf(bool solid) const
		{
			std::vector<Rect> rects;
			for (auto const& stroke : strokes)
				if (stroke.solid == solid) rects.push_back(stroke.rect);
			return rects;
		}

		double solidArea() const
		{
			return unionArea(rectsOf(true));
		}

		double outlineArea() const
		{
			return unionArea(rectsOf(false));
		}
	};

	struct LayerView
	{
		uint32_t viewLayer{ 0 };
		bool overlayEnabled{ false };
		// The selected Layer's Locations: the only apertures onto the Layer behind.
		std::vector<Rect> selectedLocations;
		std::vector<PaintedSector> sectors;
	};

	PaintedSector const* findPainted(LayerView const& view, std::string_view name)
	{
		for (auto const& sector : view.sectors)
			if (sector.name == name) return &sector;
		return nullptr;
	}

	PaintedSector const& paintedSector(LayerView const& view, std::string_view name)
	{
		auto const* sector = findPainted(view, name);
		require(sector != nullptr, ("the render snapshot has no Sector named " + std::string(name)).c_str());
		return *sector;
	}

	// Backgrounds are all named "Background", so the checks which pair a Sector
	// across two snapshots - overlay on against overlay off - pair them by index.
	PaintedSector const* findPaintedByIndex(LayerView const& view, uint32_t index)
	{
		for (auto const& sector : view.sectors)
			if (sector.index == index) return &sector;
		return nullptr;
	}

	std::vector<std::shared_ptr<const core::Sector>> locationsOn(
		core::World const& world, uint32_t layer)
	{
		std::vector<std::shared_ptr<const core::Sector>> locations;
		for (auto const& sector : world.getSectors(layer))
			if (std::dynamic_pointer_cast<const core::Location>(sector)) locations.push_back(sector);
		return locations;
	}

	//
	// Paints the whole World for one selected Layer, exactly as the renderer
	// orders it: the selected Layer first and whole, then the Layer directly
	// behind it solid through the selected Layer's apertures, then - overlay on -
	// that same Layer outlined over the selection.
	//
	// `viewLocations` stands in for the selected Layer's Sectors as the viewport
	// culled them, so a scrolled-away Location can be modelled.
	//
	// The model tracks the apertures a Transit is seen through. It does not track
	// the apertures a Door or clear Window gives a back-Layer Location, so a
	// Location on the Layer behind is only painted here by the overlay pass.
	//
	LayerView snapshotView(core::World const& world, uint32_t viewLayer,
		std::vector<std::shared_ptr<const core::Sector>> const& viewLocations, bool overlayEnabled)
	{
		LayerView view;
		view.viewLayer = viewLayer;
		view.overlayEnabled = overlayEnabled;
		for (auto const& location : viewLocations)
		{
			view.selectedLocations.push_back(rectOf(*location));
		}

		auto const layerCount = world.getLayerCount();

		std::map<uint32_t, PaintedSector> painted;
		for (uint32_t index = 0; index < world.getNumSectors(); ++index)
		{
			auto const sector = world.getSector(index);
			if (!sector) continue;

			auto& entry = painted[index];
			entry.index = index;
			entry.name = sector->getName();
			entry.sectorType = sector->getType();
			entry.layer = sector->getLayerIndex();
			entry.footprint = rectOf(*sector);
			entry.drawn = isLayerDrawn(entry.layer, viewLayer, layerCount);
		}

		for (auto const& pass : renderPasses(viewLayer, layerCount, overlayEnabled))
		{
			for (auto& [index, sector] : painted)
			{
				if (sector.layer != pass.layer) continue;

				auto const isLocation = sector.sectorType == core::SectorType::Location;
				auto const isBackground = sector.sectorType == core::SectorType::Background;

				if (pass.style == LayerRenderStyle::Aperture)
				{
					// Only a Transit is drawn through an aperture by this pass; a
					// Location reaches the screen through a threshold instead, and a
					// Background through the Window looking into it - neither of which
					// this model tracks.
					if (isLocation || isBackground) continue;

					for (auto const& aperture : transitApertures(world.getSector(index),
						viewLayer, viewLocations))
					{
						auto const piece = intersect(sector.footprint, rectOf(aperture));
						if (piece.area() > 0.0)
						{
							sector.strokes.push_back({ piece, true });
						}
					}
				}
				else if (isBackground)
				{
					// Mirrors renderSector(): filling and outlining are two separate
					// questions for a Background, so a policy which drew a border
					// over its own fill shows up here as the seam it really is.
					if (shouldFillBackground(pass.style))
						sector.strokes.push_back({ sector.footprint, true });
					if (shouldOutlineBackground(pass.style))
						sector.strokes.push_back({ sector.footprint, false });
				}
				else if (pass.style == LayerRenderStyle::Wireframe)
				{
					sector.strokes.push_back({ sector.footprint, false });
				}
				else
				{
					sector.strokes.push_back({ sector.footprint, true });
				}

				if (shouldRenderSectorAgents(sector.sectorType, pass.style))
				{
					sector.agentsVisible = true;
				}
			}
		}

		for (auto const& [index, sector] : painted)
		{
			view.sectors.push_back(sector);
		}

		return view;
	}

	LayerView snapshotView(core::World const& world, uint32_t viewLayer, bool overlayEnabled = true)
	{
		return snapshotView(world, viewLayer, locationsOn(world, viewLayer), overlayEnabled);
	}

	//
	// The Depot: four Layers deep, with a Transit of a different clipped type at
	// every depth and enough empty ground on each Layer that a Transit which
	// painted its own footprint instead of its apertures would be caught.
	//
	//   Layer 0  Front Hall / Shop / Store / Yard, the viewer's front row of Rooms
	//   Layer 1  a Ladder behind the Hall and Shop, a Stairwell behind the Store
	//            and Yard, and the Back Loft and Gallery which the Lift lands in
	//   Layer 2  the Lift behind the Loft and Gallery, and the Deep Store and Yard
	//   Layer 3  the Shuttle behind the Deep Store
	//
	void authorRenderOrderDepot(core::World& world)
	{
		while (world.getLayerCount() < 4) world.addLayer();

		world.addRoom("Front Hall", 0, 0, 0, 14, 1);
		world.addRoom("Front Shop", 0, 1, 0, 14, 1);
		world.addRoom("Front Store", 0, 2, 0, 14, 1);
		world.addRoom("Front Yard", 0, 3, 0, 14, 1);

		core::World::CreateLadderOptions ladderOptions{ 2, false, true };
		world.addLadder(1, 0, 10, ladderOptions);

		world.addRoom("Back Loft", 1, 2, 0, 8, 1);
		world.addRoom("Back Gallery", 1, 3, 0, 8, 1);

		world.addStairwell(1, 2, 8, 2, CORE_SIDE_RIGHT);

		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.stopOffsets = { 0, 1 };
		world.addLift(2, 2, 4, liftOptions);

		world.addRoom("Deep Store", 2, 0, 0, 14, 1);
		world.addRoom("Deep Yard", 2, 1, 0, 14, 1);

		core::World::CreateShuttleOptions shuttleOptions{ 2, 3, { 0, 7 }, 0 };
		world.addShuttle(3, 0, 1, 15, shuttleOptions);

		world.finishBuild();
	}

	// One Transit under test: which Layer it sits on and which Locations on the
	// Layer in front of it it is seen through.
	struct TransitExpectation
	{
		char const* name;
		uint32_t layer;
		std::vector<char const*> landingNames;
	};

	std::vector<TransitExpectation> depotTransits()
	{
		return {
			{ "Ladder", 1, { "Front Hall", "Front Shop" } },
			{ "Stairwell", 1, { "Front Store", "Front Yard" } },
			{ "Lift", 2, { "Back Loft", "Back Gallery" } },
			{ "Shuttle", 3, { "Deep Store" } },
		};
	}

	std::shared_ptr<const core::Sector> sectorByName(core::World const& world,
		std::string_view name)
	{
		for (uint32_t index = 0; index < world.getNumSectors(); ++index)
			if (world.getSector(index)->getName() == name) return world.getSector(index);
		return nullptr;
	}

	// The Door in a Location whose full shape is exactly the given aperture, or
	// null when the aperture is a bare opening with no threshold over it.
	std::shared_ptr<const core::Door> thresholdCovering(core::Sector const& location,
		TransitAperture const& aperture)
	{
		for (uint32_t i = 0; i < location.getNumObjects(); ++i)
		{
			auto const object = location.getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;

			auto const door = std::static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			core::Vector2 lo, hi;
			door->getFullShape(lo, hi);
			Rect const shape{ std::min(lo.x, hi.x), std::min(lo.y, hi.y),
				std::max(lo.x, hi.x), std::max(lo.y, hi.y) };

			if (std::abs(shape.minX - aperture.min.x) <= kAreaEpsilon
				&& std::abs(shape.minY - aperture.min.y) <= kAreaEpsilon
				&& std::abs(shape.maxX - aperture.max.x) <= kAreaEpsilon
				&& std::abs(shape.maxY - aperture.max.y) <= kAreaEpsilon)
			{
				return door;
			}
		}
		return nullptr;
	}

	//
	// The Depot really does carry every clipped Transit type behind a Layer of
	// Locations, and really does leave ground empty on those Layers for a badly
	// clipped Transit to be caught painting over.
	//
	void theDepotCarriesEveryClippedTransitBehindALayerOfLocations()
	{
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		require(world.getLayerCount() == 4, "the Depot is not four Layers deep");
		require(world.isTraversalTopologyValid(),
			("the Depot's traversal topology is invalid: " + world.getTopologyDiagnostic()).c_str());

		for (auto const& want : depotTransits())
		{
			auto const transit = std::dynamic_pointer_cast<const core::Transit>(
				sectorByName(world, want.name));
			require(transit != nullptr, ("the Depot has no Transit named " + std::string(want.name)).c_str());
			require(transit->getLayerIndex() == want.layer,
				("the Depot's " + std::string(want.name) + " is not on the Layer it is expected on").c_str());
			require(transit->getNumStops() > 0,
				("the Depot's " + std::string(want.name) + " has no stops").c_str());

			for (uint32_t stop = 0; stop < transit->getNumStops(); ++stop)
			{
				auto const landing = transit->getStop(stop).sector;
				require(landing != nullptr
					&& landing->getLayerIndex() == want.layer - 1
					&& std::find(want.landingNames.begin(), want.landingNames.end(), landing->getName())
					!= want.landingNames.end(),
					("the Depot's " + std::string(want.name) + " lands somewhere other than the Layer in front of it").c_str());
			}
		}

		// The Ladder's shaft rises past the ceiling of the Room it is seen through,
		// so part of its footprint has no aperture at all.
		auto const ladder = rectOf(*sectorByName(world, "Ladder"));
		auto const shop = rectOf(*sectorByName(world, "Front Shop"));
		Rect const aboveTheCeiling{ ladder.minX, shop.maxY, ladder.maxX, ladder.maxY };
		require(aboveTheCeiling.area() > kAreaEpsilon,
			("the Depot no longer leaves the Ladder's shaft above its landing ceiling: "
				+ describeRect(aboveTheCeiling)).c_str());

		// The Shuttle runs past the right-hand wall of the Room it is seen through.
		auto const shuttle = rectOf(*sectorByName(world, "Shuttle"));
		auto const store = rectOf(*sectorByName(world, "Deep Store"));
		Rect const pastTheWall{ store.maxX, shuttle.minY, shuttle.maxX, shuttle.maxY };
		require(pastTheWall.area() > kAreaEpsilon,
			("the Depot no longer runs the Shuttle past its landing Location: "
				+ describeRect(pastTheWall)).c_str());

		// And the Depot's front Layer is not a solid wall of Sectors: ground is
		// left empty at the right-hand end, which is what makes "and not otherwise"
		// a question with something to answer.
		double frontRight{ 0.0 };
		for (auto const& location : locationsOn(world, 0))
		{
			frontRight = std::max(frontRight, rectOf(*location).maxX);
		}
		require(frontRight + kAreaEpsilon < static_cast<double>(world.getCellsWide()),
			"the Depot no longer leaves ground empty on its front Layer");
	}

	//
	// A Transit one Layer behind the selection is painted solid where the selected
	// Layer's Locations open onto it, and only there. The overlay toggle does not
	// decide whether the Transit is visible: switching the overlay off takes away
	// its outline, not its body.
	//
	void aTransitBehindTheSelectionIsPaintedSolidThroughItsLocations()
	{
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		for (auto const& want : depotTransits())
		{
			auto const viewLayer = want.layer - 1;

			for (auto const overlay : { false, true })
			{
				auto const view = snapshotView(world, viewLayer,
					locationsOn(world, viewLayer), overlay);
				auto const& sector = paintedSector(view, want.name);
				std::string const overlayState = overlay ? "with the overlay on" : "with the overlay off";

				require(sector.drawn,
					("the " + std::string(want.name) + " behind the selection is not drawn at all "
						+ overlayState).c_str());
				require(sector.solidArea() > kAreaEpsilon,
					("the " + std::string(want.name) + " disappeared: nothing of it is painted solid "
						"through the " + std::to_string(viewLayer) + " Layer's Locations "
						+ overlayState).c_str());

				auto const insideLocations = unionAreaClippedTo(sector.rectsOf(true), view.selectedLocations);
				require(std::abs(insideLocations - sector.solidArea()) <= kAreaEpsilon,
					("the " + std::string(want.name) + " is painted solid outside the selected Layer's "
						"Locations " + overlayState + ": " + describe(sector.solidArea())
						+ " painted against " + describe(insideLocations) + " inside them").c_str());

				// Clipping must actually remove something, or the Depot has stopped
				// testing anything.
				require(sector.solidArea() + kAreaEpsilon < sector.footprint.area(),
					("the " + std::string(want.name) + " painted its whole footprint solid "
						+ overlayState + ": " + describe(sector.solidArea()) + " of "
						+ describe(sector.footprint.area()) + " was clipped away").c_str());

				// The Transit's own Agents ride inside it and are painted with it,
				// clipped to the same aperture.
				require(sector.agentsVisible,
					("the " + std::string(want.name) + "'s Agents are hidden " + overlayState).c_str());
			}
		}
	}

	//
	// The other half of the rule: the ground the selected Layer leaves empty, and
	// the ground its Locations do not reach, never show the Transit behind filled.
	// The overlay may outline that ground - that is what an x-ray is for - but it
	// must never fill it.
	//
	void aTransitIsNotPaintedWhereTheSelectedLayerDoesNotOpen()
	{
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		auto const view = snapshotView(world, 0);
		auto const& ladder = paintedSector(view, "Ladder");
		auto const& shop = rectOf(*sectorByName(world, "Front Shop"));
		auto const& ladderFootprint = ladder.footprint;

		Rect const aboveTheCeiling{ ladderFootprint.minX, shop.maxY,
			ladderFootprint.maxX, ladderFootprint.maxY };
		double paintedAbove{ 0.0 };
		for (auto const& rect : ladder.rectsOf(true))
		{
			paintedAbove += intersect(rect, aboveTheCeiling).area();
		}
		require(paintedAbove <= kAreaEpsilon,
			("the Ladder is painted solid through the Front Shop's ceiling at "
				+ describeRect(aboveTheCeiling) + ": " + describe(paintedAbove) + " of area leaked").c_str());

		// The overlay still shows that part of the Ladder, as an outline over the
		// Room it rises past.
		require(unionAreaClippedTo(ladder.rectsOf(false), { aboveTheCeiling }) > kAreaEpsilon,
			"the overlay does not outline the Ladder above its landing ceiling");

		auto const deepView = snapshotView(world, 2);
		auto const& shuttle = paintedSector(deepView, "Shuttle");
		auto const& store = rectOf(*sectorByName(world, "Deep Store"));
		Rect const pastTheWall{ store.maxX, shuttle.footprint.minY,
			shuttle.footprint.maxX, shuttle.footprint.maxY };
		double paintedPast{ 0.0 };
		for (auto const& rect : shuttle.rectsOf(true))
		{
			paintedPast += intersect(rect, pastTheWall).area();
		}
		require(paintedPast <= kAreaEpsilon,
			("the Shuttle is painted solid past its landing Location at "
				+ describeRect(pastTheWall) + ": " + describe(paintedPast) + " of area leaked").c_str());

		require(unionAreaClippedTo(shuttle.rectsOf(false), { pastTheWall }) > kAreaEpsilon,
			"the overlay does not outline the Shuttle past its landing Location");

		// Stated generally, for every Layer the viewer can select: nothing of the
		// Layer behind is filled where the selected Layer has no Location.
		for (uint32_t viewLayer = 0; viewLayer + 1 < world.getLayerCount(); ++viewLayer)
		{
			auto const layerView = snapshotView(world, viewLayer);
			for (auto const& sector : layerView.sectors)
			{
				if (sector.layer != viewLayer + 1 || sector.sectorType == core::SectorType::Location)
				{
					continue;
				}
				auto const inside = unionAreaClippedTo(sector.rectsOf(true), layerView.selectedLocations);
				require(std::abs(inside - sector.solidArea()) <= kAreaEpsilon,
					("a Transit on the Layer behind fills ground the selected Layer does not open: "
						+ sector.name).c_str());
			}
		}
	}

	//
	// The selected Layer keeps its thresholds in front of clipped transits by
	// redrawing them after the aperture pass. That occludes a Lift standing behind
	// a closed Door because the Lift's doorway aperture is exactly the Door
	// authored over it - same cell, same rectangle. If that ever stops being true,
	// a closed Door would stop covering the Shaft behind it.
	//
	void aLiftLandingDoorwayIsItsOwnThreshold()
	{
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		constexpr uint32_t viewLayer{ 1 };	// the Depot's Lift sits on Layer 2
		auto const transit = sectorByName(world, "Lift");
		require(transit != nullptr, "the Depot has no Lift");

		auto const apertures = transitApertures(transit, viewLayer, locationsOn(world, viewLayer));
		require(!apertures.empty(), "the Lift exposes no aperture on the Layer in front of it");

		for (auto const& aperture : apertures)
		{
			require(aperture.location != nullptr, "a Lift doorway aperture opens through nothing");

			auto const door = thresholdCovering(*aperture.location, aperture);
			require(door != nullptr,
				("no Door is authored over the Lift doorway at " + describeRect(rectOf(aperture))).c_str());
			require(door->getFrontSector() == aperture.location,
				"the Door over a Lift doorway is not authored on the selected Layer, so it would not occlude");
			require(door->getBackSector() == transit,
				"the Door over a Lift doorway does not open onto the Lift it covers");
		}
	}

	//
	// A Shuttle's apertures are the thresholds it actually owns. Every Door on the
	// selected Layer which opens onto the Shuttle is an aperture at its own
	// rectangle, and every aperture is such a Door - so a carriage with several
	// doors opens several doorways rather than one per landing, and nothing is
	// painted solid where the player can see no doorway. The stop's origin cell,
	// which the aperture used to be derived from, opens onto nothing.
	//
	void aShuttleOpensThroughItsOwnCarriageDoors()
	{
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		constexpr uint32_t viewLayer{ 2 };	// the Depot's Shuttle sits on Layer 3
		auto const transit = sectorByName(world, "Shuttle");
		require(transit != nullptr, "the Depot has no Shuttle");
		auto const shuttle = std::dynamic_pointer_cast<const core::ShuttleTransit>(transit);
		require(shuttle != nullptr, "the Depot's Shuttle is not a ShuttleTransit");

		std::vector<std::shared_ptr<const core::Door>> owned;
		for (auto const& location : locationsOn(world, viewLayer))
			for (uint32_t i = 0; i < location->getNumObjects(); ++i)
			{
				auto const object = location->getObject(i);
				if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
				auto const door = std::static_pointer_cast<const core::DoorSectorObject>(
					object)->getDoor();
				if (door && door->getBackSector() == transit) owned.push_back(door);
			}

		// The Depot's Shuttle is two carriages at two stops, one door each.
		require(owned.size() == 4, "the Depot's Shuttle does not own four carriage Doors");

		auto const apertures = transitApertures(transit, viewLayer, locationsOn(world, viewLayer));
		require(apertures.size() == owned.size(),
			"the Shuttle does not open one aperture per carriage Door it owns");

		for (auto const& aperture : apertures)
		{
			require(aperture.location != nullptr, "a Shuttle doorway aperture opens through nothing");

			// The aperture is exactly one owned doorway: neither wider than its
			// threshold nor shared with a Door belonging to something else.
			uint32_t covering{ 0 };
			for (auto const& door : owned)
			{
				core::Vector2 lo, hi;
				door->getFullShape(lo, hi);
				Rect const shape{ std::min(lo.x, hi.x), std::min(lo.y, hi.y),
					std::max(lo.x, hi.x), std::max(lo.y, hi.y) };
				if (std::abs(shape.minX - aperture.min.x) <= kAreaEpsilon
					&& std::abs(shape.maxX - aperture.max.x) <= kAreaEpsilon)
				{
					++covering;
				}
			}
			require(covering == 1,
				("a Shuttle aperture is not exactly one of its own carriage doorways at "
					+ describeRect(rectOf(aperture))).c_str());

			auto const door = thresholdCovering(*aperture.location, aperture);
			require(door != nullptr,
				("no Door is authored over the Shuttle doorway at "
					+ describeRect(rectOf(aperture))).c_str());
			require(door->getFrontSector() == aperture.location,
				"the Door over a Shuttle doorway is not authored on the selected Layer, so it would not occlude");
			require(door->getBackSector() == transit,
				"the Door over a Shuttle doorway does not open onto the Shuttle it covers");
		}

		// None of the apertures sits at a stop's origin cell, which is where the
		// aperture used to be and where no doorway is.
		for (uint32_t stop = 0; stop < shuttle->getNumStops(); ++stop)
		{
			auto const& landing = shuttle->getStop(stop);
			auto const stopOrigin = (float)((int)landing.sector->getCellX() + landing.sectorOffsetX);
			for (auto const& aperture : apertures)
			{
				Rect const stopCell{ stopOrigin, aperture.min.y, stopOrigin + 1.0f, aperture.max.y };
				require(intersect(stopCell, rectOf(aperture)).area() <= kAreaEpsilon,
					("a Shuttle aperture opens at stop " + std::to_string(stop)
						+ "'s origin cell, where there is no doorway").c_str());
			}
		}
	}

	//
	// Only the selected Layer and the one directly behind it reach the screen. A
	// Transit two Layers back, or one in front of the selection, is not painted at
	// all, however much its footprint would overlap what the viewer can see.
	//
	void aTransitIsNotPaintedFromAnyOtherLayer()
	{
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		auto const layerCount = world.getLayerCount();
		for (uint32_t viewLayer = 0; viewLayer < layerCount; ++viewLayer)
		{
			for (auto const overlay : { false, true })
			{
				auto const view = snapshotView(world, viewLayer,
					locationsOn(world, viewLayer), overlay);
				for (auto const& sector : view.sectors)
				{
					if (sector.sectorType == core::SectorType::Location)
					{
						continue;
					}
					auto const mayPaint = sector.layer == viewLayer || sector.layer == viewLayer + 1;
					if (!mayPaint)
					{
						require(!sector.drawn && sector.strokes.empty() && !sector.agentsVisible,
							("a Transit is painted from a Layer which is neither the selection nor the "
								"Layer behind it: " + sector.name + " on Layer " + std::to_string(sector.layer)
								+ " while Layer " + std::to_string(viewLayer) + " is selected").c_str());
						continue;
					}
					require(sector.drawn,
						("a Transit on a drawable Layer was not drawn: " + sector.name).c_str());
				}
			}
		}
	}

	//
	// An aperture belongs to a Transit's own landing. A Location on the selected
	// Layer which the Transit does not land in is not a window onto it, and neither
	// is anything which is not a Location at all.
	//
	void onlyALandingLocationOpensOntoATransit()
	{
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		for (auto const& want : depotTransits())
		{
			auto const viewLayer = want.layer - 1;
			auto const transit = sectorByName(world, want.name);
			auto const apertures = transitApertures(transit, viewLayer, locationsOn(world, viewLayer));
			require(!apertures.empty(),
				("the " + std::string(want.name) + " exposes no aperture on the Layer in front of it").c_str());

			for (auto const& aperture : apertures)
			{
				require(aperture.location != nullptr
					&& std::find(want.landingNames.begin(), want.landingNames.end(), aperture.location->getName())
					!= want.landingNames.end(),
					("the " + std::string(want.name) + " opens through a Location it does not land in").c_str());
			}

			// Every other Location on the selected Layer keeps its floor: none of
			// the Transit's apertures reach into it.
			for (auto const& location : locationsOn(world, viewLayer))
			{
				auto const isLanding = std::find(want.landingNames.begin(), want.landingNames.end(),
					location->getName()) != want.landingNames.end();
				if (isLanding) continue;
				for (auto const& aperture : apertures)
				{
					require(!overlaps(rectOf(aperture), rectOf(*location)),
						("the " + std::string(want.name) + " opens through " + location->getName()
							+ ", a Location it does not land in").c_str());
				}
			}

			// Nor does a Transit on the selected Layer act as an aperture: the
			// deeper Transit must not appear through it.
			for (auto const& other : world.getSectors(viewLayer))
			{
				if (other->getType() == core::SectorType::Location) continue;
				for (auto const& aperture : apertures)
				{
					require(!overlaps(rectOf(aperture), rectOf(*other)),
						("the " + std::string(want.name) + " opens through " + other->getName()
							+ ", a Transit rather than a Location").c_str());
				}
			}
		}
	}

	//
	// A Transit opens onto one Layer only: the one directly in front of it. Asked
	// about any other Layer - its own, the front-most, the back-most - it exposes
	// no aperture, which is what stops a Transit being seen through a Layer it is
	// not behind.
	//
	void aTransitOnlyOpensOntoTheLayerInFrontOfIt()
	{
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		for (auto const& want : depotTransits())
		{
			auto const transit = sectorByName(world, want.name);
			for (uint32_t viewLayer = 0; viewLayer < world.getLayerCount(); ++viewLayer)
			{
				auto const apertures = transitApertures(transit, viewLayer,
					locationsOn(world, viewLayer));
				if (viewLayer + 1 == want.layer)
				{
					require(!apertures.empty(),
						("the " + std::string(want.name) + " does not open onto the Layer in front of it").c_str());
					continue;
				}
				require(apertures.empty(),
					("the " + std::string(want.name) + " exposes an aperture to Layer "
						+ std::to_string(viewLayer) + ", which it is not directly behind").c_str());
			}
		}
	}

	//
	// A Staircase is the one Transit whose apertures are the selected Layer's
	// Locations rather than its own landings: it is drawn across whatever the
	// viewer has in front of it. Its body is still cut to them - never filled
	// where no Location reaches, and never across the gap between two stacked
	// Locations whose ceilings and floors do not meet.
	//
	void aStaircaseIsPaintedAcrossTheLocationsInView()
	{
		core::World world("Staircase render order", 12, 3);
		while (world.getLayerCount() < 2) world.addLayer();
		world.addCorridor(0, 0, 6);
		world.addCorridor(1, 0, 7);
		auto const room = world.addRoom("Upper room", 0, 1, 7, 3, 1);
		world.removeLocationWall(room, 0, CORE_SIDE_LEFT);
		world.addStaircase(1, 0, 5,
			core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.0f });
		world.finishBuild();
		require(world.isTraversalTopologyValid(),
			("the Staircase World's traversal topology is invalid: "
				+ world.getTopologyDiagnostic()).c_str());

		for (auto const overlay : { false, true })
		{
			auto const view = snapshotView(world, 0, locationsOn(world, 0), overlay);
			auto const& staircase = paintedSector(view, "Staircase");
			std::string const overlayState = overlay ? "with the overlay on" : "with the overlay off";

			require(staircase.solidArea() > kAreaEpsilon,
				("the Staircase disappeared behind the selected Layer " + overlayState).c_str());
			auto const inside = unionAreaClippedTo(staircase.rectsOf(true), view.selectedLocations);
			require(std::abs(inside - staircase.solidArea()) <= kAreaEpsilon,
				("the Staircase is painted solid outside the selected Layer's Locations "
					+ overlayState).c_str());
			require(staircase.solidArea() + kAreaEpsilon < staircase.footprint.area(),
				("the Staircase painted its whole span solid " + overlayState
					+ " rather than only the Locations it crosses").c_str());
			require(staircase.agentsVisible,
				("the Staircase's Agents are hidden " + overlayState).c_str());
		}

		// With nothing of the selected Layer in front of it, the Staircase has no
		// aperture to be seen through at all.
		std::vector<std::shared_ptr<const core::Sector>> none;
		auto const emptyView = snapshotView(world, 0, none, true);
		require(paintedSector(emptyView, "Staircase").solidArea() <= kAreaEpsilon,
			"the Staircase is painted solid with no Location of the selected Layer in view");

		// With only the lower Corridor in view, none of it reaches above that
		// Corridor's ceiling.
		std::vector<std::shared_ptr<const core::Sector>> lowerOnly;
		Rect lowerCeiling{};
		for (auto const& sector : world.getSectors(0))
		{
			if (sector->getCellY() != 0) continue;
			lowerOnly.push_back(sector);
			lowerCeiling = rectOf(*sector);
		}
		require(!lowerOnly.empty(), "the Staircase World has no lower Corridor");
		auto const lowerView = snapshotView(world, 0, lowerOnly, true);
		auto const& lowerStaircase = paintedSector(lowerView, "Staircase");
		require(lowerStaircase.solidArea() > kAreaEpsilon,
			"the Staircase vanished when only the lower Corridor is in view");
		for (auto const& rect : lowerStaircase.rectsOf(true))
		{
			require(rect.maxY <= lowerCeiling.maxY + kAreaEpsilon,
				("the Staircase is painted solid above the only Location in view: "
					+ describeRect(rect) + " against ceiling " + describe(lowerCeiling.maxY)).c_str());
		}
	}

	// A Staircase's procedural steps are drawn over a plain shaft surface. Using
	// the decorative staircase atlas region as the Sector surface paints a second,
	// fixed stair image underneath them on the Staircase's own Layer.
	void aStaircaseUsesThePlainShaftSurface()
	{
		core::World world("Staircase shaft surface", 12, 3);
		while (world.getLayerCount() < 2) world.addLayer();
		world.addCorridor(0, 0, 6);
		world.addCorridor(1, 0, 7);
		auto const room = world.addRoom("Upper room", 0, 1, 7, 3, 1);
		world.removeLocationWall(room, 0, CORE_SIDE_LEFT);
		world.addStaircase(1, 0, 5,
			core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.0f });
		world.finishBuild();

		auto const staircase = sectorByName(world, "Staircase");
		require(staircase != nullptr, "the Staircase Sector could not be found");

		SectorTileset tileset;
		tileset.width = 100;
		tileset.height = 100;
		tileset.surfaces.emplace("stairwell", SectorTileRegion{ 0, 0, 10, 10 });
		tileset.surfaces.emplace("staircase", SectorTileRegion{ 50, 50, 10, 10 });
		setSectorTileset(std::move(tileset), reinterpret_cast<ImTextureID>(1));

		WorldDrawList commands({ { -100000.0f, -100000.0f }, { 100000.0f, 100000.0f } });
		renderSector(staircase, 1, LayerRenderStyle::Solid, false,
			ImColor(IM_COL32_WHITE), &commands);

		bool foundSurface = false;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::SectorAtlas) continue;
			foundSurface = true;
			for (auto const& uv : triangle->texcoords)
				require(uv.x < 0.2f && uv.y < 0.2f,
					"the Staircase Sector used decorative stair artwork instead of the plain shaft surface");
		}
		clearSectorTileset();
		require(foundSurface, "the Staircase Sector emitted no textured shaft surface");
	}

	//
	// The overlay is an outline. It never fills a Sector, never draws a Transit's
	// own geometry, and never shows the Agents standing inside it.
	//
	void theOverlayNeverLeaksSolidGeometryOrAgents()
	{
		std::vector<LayerRenderStyle> const styles{
			LayerRenderStyle::Hidden, LayerRenderStyle::Solid,
			LayerRenderStyle::Wireframe, LayerRenderStyle::Aperture };
		std::vector<core::SectorType> const types{
			core::SectorType::Location, core::SectorType::Ladder, core::SectorType::Lift,
			core::SectorType::Shuttle, core::SectorType::Stairwell, core::SectorType::Staircase };

		for (auto const style : styles)
		{
			auto const solid = style == LayerRenderStyle::Solid || style == LayerRenderStyle::Aperture;
			require(isDrawnSolid(style) == solid,
				"only the selected Layer and an aperture pass are painted solid");
			require(shouldRenderLadderGeometry(style) == solid,
				"Ladder geometry is not following the solid-fill policy");
			require(shouldRenderStairwellGeometry(style) == solid,
				"Stairwell geometry is not following the solid-fill policy");

			// Clipping belongs to the aperture pass alone. The selected Layer draws
			// its own Transits whole, and the overlay outlines the whole Layer
			// behind over the selection - that is the x-ray it exists to give.
			require(shouldClipTransitToApertures(style) == (style == LayerRenderStyle::Aperture),
				"Transit clipping is not following the Layer style");

			for (auto const type : types)
			{
				require(shouldRenderSectorAgents(type, style) == solid,
					"Agents are exposed by a pass which must not show them");
			}
		}

		require(shouldRenderForeContentAfterTransit(core::SectorType::Ladder)
				&& !shouldRenderForeContentAfterTransit(core::SectorType::Lift)
				&& !shouldRenderForeContentAfterTransit(core::SectorType::Shuttle),
			"a Ladder is not drawn behind the contents of the Location it lands in");
		require(shouldRenderStaircaseAfterSector(core::SectorType::Location)
				&& !shouldRenderStaircaseAfterSector(core::SectorType::Staircase),
			"a Staircase is not ordered after the Locations it crosses");

		// The same statement against a live picture: turning the overlay on adds
		// outlines and nothing else. Every Sector's filled area, and whether any
		// pass was allowed to paint its Agents, is exactly what it was with the
		// overlay off.
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		for (uint32_t viewLayer = 0; viewLayer < world.getLayerCount(); ++viewLayer)
		{
			auto const without = snapshotView(world, viewLayer,
				locationsOn(world, viewLayer), false);
			auto const with = snapshotView(world, viewLayer,
				locationsOn(world, viewLayer), true);

			for (auto const& sector : without.sectors)
			{
				auto const& overlaid = paintedSector(with, sector.name);
				require(std::abs(sector.solidArea() - overlaid.solidArea()) <= kAreaEpsilon,
					("the overlay changed how much of a Sector is filled: " + sector.name).c_str());
				require(sector.agentsVisible == overlaid.agentsVisible,
					("the overlay changed whether a Sector's Agents are painted: " + sector.name).c_str());
			}
		}
	}

	//
	// The selected Layer paints itself whole, Transits included; the Layer behind
	// contributes its body through the selected Layer's apertures and, overlay
	// on, its outlines over the selection.
	//
	void theSelectedLayerPaintsItselfWhole()
	{
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		for (uint32_t viewLayer = 0; viewLayer < world.getLayerCount(); ++viewLayer)
		{
			auto const view = snapshotView(world, viewLayer);
			uint32_t selected{ 0 };
			for (auto const& sector : view.sectors)
			{
				if (sector.layer != viewLayer) continue;
				++selected;
				require(sector.drawn
						&& std::abs(sector.solidArea() - sector.footprint.area()) <= kAreaEpsilon,
					("the selected Layer does not paint one of its Sectors whole: " + sector.name).c_str());
				require(sector.agentsVisible,
					("the selected Layer hides one of its own Sectors' Agents: " + sector.name).c_str());
			}
			require(selected > 0, "the selected Layer painted no Sectors at all");

			// The Agents riding a Transit on the Layer behind are visible from the
			// moment the Transit itself is drawn solid - through the selected
			// Layer's apertures as well as from the Transit's own Layer.
			auto const& lift = paintedSector(view, "Lift");
			require(lift.agentsVisible == (viewLayer == 1 || viewLayer == 2),
				"a Lift rider's visibility does not follow whether the Lift is drawn solid");
		}
	}

	//
	// The overlay is what an x-ray is for: it outlines the whole Layer behind over
	// the selection, including the ground its solid pass could never reach. It
	// never fills, and switching it off leaves the Layer behind's body painted.
	//
	void theOverlayOutlinesTheWholeLayerBehind()
	{
		core::World world("Render order depot", 16, 4);
		authorRenderOrderDepot(world);

		for (uint32_t viewLayer = 0; viewLayer + 1 < world.getLayerCount(); ++viewLayer)
		{
			auto const without = snapshotView(world, viewLayer,
				locationsOn(world, viewLayer), false);
			auto const with = snapshotView(world, viewLayer,
				locationsOn(world, viewLayer), true);

			uint32_t outlined{ 0 };
			for (auto const& sector : with.sectors)
			{
				if (sector.layer != viewLayer + 1) continue;
				++outlined;

				require(std::abs(sector.outlineArea() - sector.footprint.area()) <= kAreaEpsilon,
					("the overlay does not outline one Sector of the Layer behind whole: "
						+ sector.name).c_str());
				require(paintedSector(without, sector.name).outlineArea() <= kAreaEpsilon,
					("the Layer behind is outlined with the overlay off: " + sector.name).c_str());
			}
			require(outlined > 0, "the Layer behind contributed no outlined Sectors");
		}
	}

	//
	// The pass order itself: the selected Layer first and solid, then the Layer
	// directly behind drawn solid through apertures, then one wireframe overlay of
	// that same Layer, and nothing after that. renderPasses() is the very function
	// renderWorld() drives, so this is the renderer's own order.
	//
	void theRenderPassOrderDrawsTheSelectedLayerFirst()
	{
		for (uint32_t layerCount = 2; layerCount <= 4; ++layerCount)
		{
			for (uint32_t viewLayer = 0; viewLayer < layerCount; ++viewLayer)
			{
				auto const behind = viewLayer + 1 < layerCount;
				auto const passes = renderPasses(viewLayer, layerCount, true);

				require(!passes.empty(), "a selected Layer produced no render pass");
				require(passes.front().layer == viewLayer
						&& passes.front().style == LayerRenderStyle::Solid,
					"the first render pass is not the selected Layer drawn solid");

				auto const expectedPasses = behind ? 3u : 1u;
				require(passes.size() == expectedPasses,
					"the render pass list is not the selected Layer, the Layer behind through its "
					"apertures, and one overlay");

				if (behind)
				{
					require(passes[1].layer == viewLayer + 1
							&& passes[1].style == LayerRenderStyle::Aperture,
						"the Layer behind is not drawn solid through apertures straight after the "
						"selected Layer");
					require(passes[2].layer == viewLayer + 1
							&& passes[2].style == LayerRenderStyle::Wireframe,
						"the overlay does not outline the Layer directly behind the selection");
				}

				// Turning the overlay off takes the outline away and nothing else:
				// the Layer behind is still drawn solid through its apertures.
				auto const noOverlay = renderPasses(viewLayer, layerCount, false);
				require(noOverlay.size() == (behind ? 2u : 1u),
					"disabling the wireframe overlay removed more than the overlay pass");
				if (behind)
				{
					require(noOverlay[1].layer == viewLayer + 1
							&& noOverlay[1].style == LayerRenderStyle::Aperture,
						"disabling the wireframe overlay also removed the Layer behind's solid pass");
				}
			}
		}
	}

	void digestValue(uint64_t& digest, uint64_t value)
	{
		digest ^= value;
		digest *= 1099511628211ULL;
	}

	uint64_t snapshotDigest(core::World const& world)
	{
		uint64_t digest{ 1469598103934665603ULL };
		for (uint32_t viewLayer = 0; viewLayer < world.getLayerCount(); ++viewLayer)
		{
			for (uint32_t overlay = 0; overlay < 2; ++overlay)
			{
				digestValue(digest, viewLayer);
				digestValue(digest, overlay);
				auto const view = snapshotView(world, viewLayer,
					locationsOn(world, viewLayer), overlay != 0);
				for (auto const& sector : view.sectors)
				{
					digestValue(digest, sector.index);
					digestValue(digest, sector.layer);
					digestValue(digest, static_cast<uint64_t>(sector.sectorType));
					digestValue(digest, sector.drawn ? 1u : 0u);
					digestValue(digest, sector.agentsVisible ? 1u : 0u);
					for (auto const& stroke : sector.strokes)
					{
						digestValue(digest, stroke.solid ? 1u : 0u);
						digestValue(digest, std::bit_cast<uint64_t>(stroke.rect.minX));
						digestValue(digest, std::bit_cast<uint64_t>(stroke.rect.minY));
						digestValue(digest, std::bit_cast<uint64_t>(stroke.rect.maxX));
						digestValue(digest, std::bit_cast<uint64_t>(stroke.rect.maxY));
					}
				}
			}
		}
		return digest;
	}

	//
	// A clear Window's Aperture pass is handed the Background's own colour, not
	// the generic back-layer tint: the glass shows what is actually behind it.
	// The pass that receives that colour is the Aperture pass renderPasses()
	// orders for the Layer directly behind the selection, and the colour follows
	// the Background - re-colour it and the glass follows. A Window looking into
	// anything other than a Background still takes the generic tint, so the
	// arrangement that must not move is measured too.
	//
	void aClearWindowShowsItsBackgroundsOwnColour()
	{
		core::World world("Backdrop render order", 12, 2);
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Front", 0, 0, 0, 12, 1);
		auto const backdropIndex = world.addBackground(1, 0, 0, 6, 1, { 255, 128, 0 });
		world.addRoom("Behind", 1, 0, 6, 6, 1);

		auto const looking = world.addSectorWindow(0, 0, 1, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		auto const tinted = world.addSectorWindow(0, 0, 7, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		world.finishBuild();

		require(looking.object != nullptr && tinted.object != nullptr,
			"a clear Window was not created");
		require(looking.object->getStyle() == core::Window::Style::Clear
			&& tinted.object->getStyle() == core::Window::Style::Clear,
			"a Window under test is not a clear Window");

		// The Aperture pass for the Layer behind the selection is the pass that
		// receives the fill colour; renderPasses() is the renderer's own order.
		auto const passes = renderPasses(0, world.getLayerCount(), false);
		bool hasAperturePass{ false };
		for (auto const& pass : passes)
		{
			hasAperturePass = hasAperturePass
				|| (pass.layer == 1 && pass.style == LayerRenderStyle::Aperture);
		}
		require(hasAperturePass,
			"the Layer behind the selection is not drawn through apertures, so no fill colour is handed down");

		// The Window over the backdrop really looks into the Background.
		auto const back = looking.object->getBackSector();
		require(back != nullptr && back->getType() == core::SectorType::Background,
			"the Window over the backdrop does not look into a Background");
		require(back->getIndex() == backdropIndex, "the Window looks into the wrong Background");

		// The Aperture pass is handed the Background's own colour.
		auto const fill = apertureFillColour(*back);
		require(fill.has_value(), "a Background back Sector offers no aperture fill colour");
		require(*fill == core::BackgroundColour{ 255, 128, 0 },
			"the aperture fill colour is not the Background's own colour");

		// The glass follows the Background: re-colour it and the fill follows,
		// which is the headless half of the hand-edited-YAML manual test.
		auto const recolourable = std::make_shared<core::Background>(
			"Backdrop", 1u, backdropIndex, 0u, 0u, 6u, 1u, core::BackgroundColour{ 255, 128, 0 });
		require(apertureFillColour(*recolourable) == core::BackgroundColour{ 255, 128, 0 },
			"a freshly built Background does not offer its own colour");
		recolourable->setColour({ 12, 34, 56 });
		require(apertureFillColour(*recolourable) == core::BackgroundColour{ 12, 34, 56 },
			"the aperture fill colour did not follow the Background's recolour");

		// The control: a clear Window looking into a Room carries no colour of
		// its own, so the caller keeps the generic back-layer tint.
		auto const tintedBack = tinted.object->getBackSector();
		require(tintedBack != nullptr && tintedBack->getType() != core::SectorType::Background,
			"the control Window is not looking into something other than a Background");
		require(!apertureFillColour(*tintedBack).has_value(),
			"a non-Background back Sector claims a fill colour of its own; the tint would be overridden");
	}

	//
	// A two-Layer backdrop for the Background outline rules: one Room across the
	// left of the front Layer with ground left empty on the right, and two
	// adjacent same-colour Backgrounds spanning the Layer behind. They meet at
	// x = 6, which is the seam, and the empty ground past the Room's wall at
	// x = 8 is ground no aperture reaches.
	//
	struct BackdropLayout
	{
		uint32_t leftIndex{ 0 };
		uint32_t rightIndex{ 0 };
	};

	BackdropLayout authorBackgroundBackdrop(core::World& world)
	{
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Front", 0, 0, 0, 8, 1);

		BackdropLayout layout;
		layout.leftIndex = world.addBackground(1, 0, 0, 6, 1, { 200, 60, 40 });
		layout.rightIndex = world.addBackground(1, 0, 6, 6, 1, { 200, 60, 40 });
		world.finishBuild();
		return layout;
	}

	//
	// Per LayerRenderStyle: a Background fills exactly when its Layer is drawn
	// solid, is outlined by the wireframe overlay alone, and never does both in
	// one pass. This is #35's narrowing of the umbrella's "no per-sector border"
	// written down as one rule which the renderer and these checks read together.
	//
	void aBackgroundFillsSolidAndIsOutlinedOnlyByTheOverlay()
	{
		std::vector<LayerRenderStyle> const styles{
			LayerRenderStyle::Hidden, LayerRenderStyle::Solid,
			LayerRenderStyle::Wireframe, LayerRenderStyle::Aperture };

		for (auto const style : styles)
		{
			require(shouldFillBackground(style) == isDrawnSolid(style),
				"a Background does not fill exactly when its Layer is drawn solid");
			require(shouldOutlineBackground(style) == (style == LayerRenderStyle::Wireframe),
				"a Background is not outlined by the wireframe overlay alone");
			// A border drawn over a Background's own fill is exactly the seam the
			// Solid pass must not show between adjacent same-colour Backgrounds.
			require(!(shouldFillBackground(style) && shouldOutlineBackground(style)),
				"one pass both fills a Background and borders it");
		}

		require(!shouldFillBackground(LayerRenderStyle::Hidden)
				&& !shouldOutlineBackground(LayerRenderStyle::Hidden),
			"the Hidden style paints a Background at all");
	}

	//
	// The same rule over a live picture: a Background on the Layer behind is
	// outlined whole over the selection - including the ground in front which no
	// aperture reaches - and fills nothing. With the overlay off it contributes
	// no outline whatsoever, which is the imgui manual test read headlessly.
	//
	void aBackgroundBehindTheSelectionIsOutlinedWholeByTheOverlay()
	{
		core::World world("Backdrop render order", 12, 2);
		auto const layout = authorBackgroundBackdrop(world);
		require(world.isTraversalTopologyValid(),
			("the backdrop World's traversal topology is invalid: "
				+ world.getTopologyDiagnostic()).c_str());

		constexpr uint32_t viewLayer{ 0 };
		auto const with = snapshotView(world, viewLayer, locationsOn(world, viewLayer), true);
		auto const without = snapshotView(world, viewLayer, locationsOn(world, viewLayer), false);

		uint32_t behind{ 0 };
		for (auto const& sector : with.sectors)
		{
			if (sector.sectorType != core::SectorType::Background || sector.layer != viewLayer + 1)
			{
				continue;
			}
			auto const where = " Background at index " + std::to_string(sector.index);
			++behind;

			require(sector.drawn, ("a Background on the Layer behind is not drawn at all" + where).c_str());
			require(std::abs(sector.outlineArea() - sector.footprint.area()) <= kAreaEpsilon,
				("the overlay does not outline a Background whole:" + where).c_str());
			require(sector.solidArea() <= kAreaEpsilon,
				("the overlay filled a Background over the selection:" + where).c_str());

			auto const* plain = findPaintedByIndex(without, sector.index);
			require(plain != nullptr, ("a Background vanished with the overlay off" + where).c_str());
			require(plain->outlineArea() <= kAreaEpsilon,
				("a Background is outlined with the overlay off:" + where).c_str());
		}
		require(behind == 2, "the backdrop does not carry two Backgrounds on the Layer behind");

		// The overlay is not clipped to the apertures the selected Layer has: the
		// front Room stops at its own wall while the Backgrounds run on past it,
		// and that ground is still outlined.
		auto const front = rectOf(*sectorByName(world, "Front"));
		auto const right = rectOf(*world.getSector(layout.rightIndex));
		Rect const noAperture{ front.maxX, right.minY, right.maxX, right.maxY };
		require(noAperture.area() > kAreaEpsilon,
			("the backdrop no longer leaves Background ground with no Location in front of it: "
				+ describeRect(noAperture)).c_str());

		double outlined{ 0.0 };
		for (auto const& sector : with.sectors)
		{
			if (sector.sectorType != core::SectorType::Background) continue;
			outlined += unionAreaClippedTo(sector.rectsOf(false), { noAperture });
		}
		require(std::abs(outlined - noAperture.area()) <= kAreaEpsilon,
			("the overlay does not outline the Background over ground the selected Layer does not open: "
				+ describe(outlined) + " against " + describe(noAperture.area())).c_str());
	}

	//
	// The other half of the narrowing: with the Backgrounds on the selected
	// Layer, the Solid pass fills them and draws no border of any kind, so two
	// adjacent same-colour Backgrounds meet with no visible seam. A band across
	// their shared edge is filled, and nothing is drawn over it.
	//
	void adjacentBackgroundsMeetWithoutASeam()
	{
		core::World world("Backdrop render order", 12, 2);
		auto const layout = authorBackgroundBackdrop(world);

		constexpr uint32_t viewLayer{ 1 };
		auto const view = snapshotView(world, viewLayer, locationsOn(world, viewLayer), false);

		uint32_t count{ 0 };
		std::vector<Rect> fills;
		std::vector<Rect> borders;
		for (auto const& sector : view.sectors)
		{
			if (sector.sectorType != core::SectorType::Background || sector.layer != viewLayer)
			{
				continue;
			}
			auto const where = " Background at index " + std::to_string(sector.index);
			++count;

			require(std::abs(sector.solidArea() - sector.footprint.area()) <= kAreaEpsilon,
				("the selected Layer does not fill one of its Backgrounds whole:" + where).c_str());
			require(sector.rectsOf(false).empty(),
				("the Solid pass draws a border around a Background:" + where).c_str());

			for (auto const& rect : sector.rectsOf(true)) fills.push_back(rect);
			for (auto const& rect : sector.rectsOf(false)) borders.push_back(rect);
		}
		require(count == 2, "the backdrop does not carry two Backgrounds on the selected Layer");

		auto const left = rectOf(*world.getSector(layout.leftIndex));
		auto const right = rectOf(*world.getSector(layout.rightIndex));
		require(std::abs(left.maxX - right.minX) <= kAreaEpsilon,
			("the two Backgrounds no longer share an edge: " + describeRect(left) + " against "
				+ describeRect(right)).c_str());

		Rect const seam{ left.maxX - 0.5, left.minY, right.minX + 0.5, left.maxY };
		require(std::abs(unionAreaClippedTo(fills, { seam }) - seam.area()) <= kAreaEpsilon,
			("the two Backgrounds do not fill the ground where they meet: " + describeRect(seam)).c_str());
		require(unionArea(borders) <= kAreaEpsilon,
			"a border is drawn across the seam between two adjacent Backgrounds");
	}

	//
	// The narrowing leaves selection alone: a selected Background still takes the
	// highlight exactly as a Location does, and no other pass, Layer, or
	// selection mode draws one.
	//
	void aSelectedBackgroundStillTakesTheSelectionHighlight()
	{
		require(shouldHighlightSelectedSector(LayerRenderStyle::Solid, 1u, 1u, true),
			"a selected Background on the selected Layer loses its selection highlight");
		require(shouldHighlightSelectedSector(LayerRenderStyle::Solid, 0u, 0u, true),
			"the highlight no longer follows a Location too, so it is not type-agnostic");
		require(!shouldHighlightSelectedSector(LayerRenderStyle::Wireframe, 1u, 0u, true),
			"the overlay highlights a Background of the Layer behind");
		require(!shouldHighlightSelectedSector(LayerRenderStyle::Aperture, 1u, 0u, true),
			"an aperture pass highlights the Background it shows");
		require(!shouldHighlightSelectedSector(LayerRenderStyle::Solid, 1u, 0u, true),
			"a Sector is highlighted while it is not on the selected Layer");
		require(!shouldHighlightSelectedSector(LayerRenderStyle::Solid, 1u, 1u, false),
			"a highlight is drawn outside Sector selection mode");
	}

	//
	// Ticket #37: a Window spanning several Backgrounds composites them.
	//
	// The visible regions are derived from the back Layer's cell grid, not
	// from the Window's single back Sector, which #36 made non-authoritative
	// for such a span. Each region carries the Background it shows and the
	// exact world-space rectangle it is clipped to: the intersection of the
	// Window rect and that Background's own rect. The seam between two
	// Backgrounds therefore lands exactly on the cell boundary between them
	// - no bleed past it, no gap before it, no seam line across it - and the
	// rects are world-space, so the composite is pinned to the world rather
	// than to whatever the viewport happens to show.
	//
	void aMultiBackgroundApertureCompositesEachBackgroundClipped()
	{
		// Sky blue and car-park grey, side by side behind one 4-wide Window -
		// the imgui manual test of the ticket body, authored headlessly.
		constexpr core::BackgroundColour kSky{ 96, 128, 160 };
		constexpr core::BackgroundColour kCarPark{ 112, 112, 112 };

		core::World world("Multi-background aperture", 12, 2);
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Front", 0, 0, 0, 8, 1);
		auto const skyIndex = world.addBackground(1, 0, 0, 6, 1, kSky);
		auto const carIndex = world.addBackground(1, 0, 6, 6, 1, kCarPark);

		auto const spanning = world.addSectorWindow(0, 0, 4, 4, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		world.finishBuild();

		require(spanning.object != nullptr, "the spanning Window was not created");

		// The Window's single back Sector is the first Background in the span.
		// A renderer that trusted it would paint the whole glass sky blue and
		// the car park would be invisible through its half of the Window.
		auto const back = spanning.object->getBackSector();
		require(back != nullptr && back->getType() == core::SectorType::Background
				&& back->getIndex() == skyIndex,
			"expected the spanning Window's back Sector to be the first Background, not the whole truth");

		core::Vector2 wLo, wHi;
		spanning.object->getFullShape(wLo, wHi);
		// The glass is inset from the cell grid (CORE_WINDOW_X_INSET), so the
		// aperture's own edges are not cell edges; the seam between the two
		// Backgrounds still lands exactly on the cell boundary at x = 6.
		require(wLo.x > 4.0f && wHi.x < 8.0f && wLo.y > 0.0f && wHi.y < 1.0f,
			"the spanning Window is not where it was authored");

		auto const regions = backgroundApertureRegions(world, 1, wLo, wHi);
		require(regions.size() == 2,
			"a Window over two Backgrounds did not composite two regions");

		// Ordered by the cell grid's left-to-right sweep of first appearance.
		require(regions[0].background->getIndex() == skyIndex
				&& regions[1].background->getIndex() == carIndex,
			"the composite regions are not ordered by the cell grid sweep");

		// Clip bounds: each region is the Window rect clipped to its own
		// Background, so the split lands exactly on the cell boundary at x = 6.
		require(regions[0].min.x == wLo.x && regions[0].min.y == wLo.y
				&& regions[0].max.x == 6.0f && regions[0].max.y == wHi.y,
			"the sky Background is not clipped to the Window's left half up to the seam");
		require(regions[1].min.x == 6.0f && regions[1].min.y == wLo.y
				&& regions[1].max.x == wHi.x && regions[1].max.y == wHi.y,
			"the car-park Background is not clipped to the Window's right half from the seam");

		// No bleed and no gap: the two regions share the seam edge exactly, do
		// not overlap, and together cover the aperture whole.
		require(regions[0].max.x == regions[1].min.x,
			"the two regions do not meet exactly on the cell boundary");
		auto const overlap = intersect(rectOf(regions[0]), rectOf(regions[1]));
		require(overlap.area() <= kAreaEpsilon, "the two composite regions overlap - one bleeds past the seam");
		auto const aperture = rectOf(wLo, wHi);
		require(std::abs(rectOf(regions[0]).area() + rectOf(regions[1]).area() - aperture.area()) <= kAreaEpsilon,
			"the composite does not cover the aperture - a gap stands behind the glass");

		// Each region fills with its own Background's colour, not a shared tint:
		// the #34 rule read once per region of the composite.
		auto const skyFill = apertureFillColour(*regions[0].background);
		auto const carFill = apertureFillColour(*regions[1].background);
		require(skyFill.has_value() && *skyFill == kSky,
			"the sky region does not fill with the sky Background's own colour");
		require(carFill.has_value() && *carFill == kCarPark,
			"the car-park region does not fill with the car-park Background's own colour");

		// The composite is world-space: the helper reads the cell grid, not the
		// viewport, so scrolling can only ever move the seam with the
		// Backgrounds, never with the screen. Any sub-rect of the aperture sees
		// the same seam at the same world x.
		for (auto const& region : regions)
		{
			core::Vector2 bgLo, bgHi;
			region.background->getBounds(bgLo, bgHi);
			require(region.min.x == std::max(bgLo.x, wLo.x) && region.max.x == std::min(bgHi.x, wHi.x),
				"a composite region is not the world-space intersection of aperture and Background");
		}

		// A Window wholly inside one Background yields one region clipped to
		// the Window itself - the seam next door changes nothing. And a Window
		// whose glass stops short of the seam takes no sliver of the
		// neighbour: the clip ends at the Window's own edge, inside the sky
		// Background, and the car park contributes nothing.
		core::World near("Near-seam apertures", 12, 2);
		while (near.getLayerCount() < 2) near.addLayer();
		near.addRoom("Front", 0, 0, 0, 8, 1);
		near.addBackground(1, 0, 0, 6, 1, kSky);
		near.addBackground(1, 0, 6, 6, 1, kCarPark);
		auto const single = near.addSectorWindow(0, 0, 1, 1, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		auto const abutting = near.addSectorWindow(0, 0, 2, 4, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		near.finishBuild();

		require(single.object != nullptr, "the single-Background Window was not created");
		core::Vector2 sLo, sHi;
		single.object->getFullShape(sLo, sHi);
		auto const singleRegions = backgroundApertureRegions(world, 1, sLo, sHi);
		require(singleRegions.size() == 1
				&& singleRegions[0].background->getIndex() == skyIndex,
			"a Window wholly over one Background did not yield exactly that Background's region");
		require(singleRegions[0].min.x == sLo.x && singleRegions[0].max.x == sHi.x,
			"the single region is not clipped to the Window itself");

		// A Window whose glass stops short of the seam takes no sliver of the
		// neighbour: the clip ends at the Window's own edge, inside the sky
		// Background, and the car park contributes nothing.
		require(abutting.object != nullptr, "the seam-abutting Window was not created");
		core::Vector2 aLo, aHi;
		abutting.object->getFullShape(aLo, aHi);
		require(aHi.x < 6.0f, "the abutting Window's glass does not stop short of the seam");
		auto const abuttingRegions = backgroundApertureRegions(world, 1, aLo, aHi);
		require(abuttingRegions.size() == 1
				&& abuttingRegions[0].background->getIndex() == skyIndex
				&& abuttingRegions[0].max.x == aHi.x,
			"a Window stopping short of the seam bleeds a region of the neighbour in");

		// The order is deterministic: the same World swept twice gives the
		// same composite, which is what keeps the two-pass renderer stable.
		auto const again = backgroundApertureRegions(world, 1, wLo, wHi);
		require(again.size() == regions.size()
				&& again[0].background->getIndex() == regions[0].background->getIndex()
				&& again[1].background->getIndex() == regions[1].background->getIndex(),
			"the composite order is not deterministic across sweeps");
	}

	//
	// The composite spans however many Backgrounds the aperture crosses, and
	// yields nothing at all when the aperture looks into no Background - an
	// ordinary Window into a Room keeps the caller's single-sector tint path.
	//
	void aMultiBackgroundApertureSpansEveryBackgroundAndSkipsNone()
	{
		core::World world("Three-background aperture", 12, 2);
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Front", 0, 0, 0, 12, 1);
		auto const firstIndex = world.addBackground(1, 0, 0, 4, 1, { 10, 20, 30 });
		auto const secondIndex = world.addBackground(1, 0, 4, 4, 1, { 40, 50, 60 });
		auto const thirdIndex = world.addBackground(1, 0, 8, 4, 1, { 70, 80, 90 });

		// A 6-wide Window from x = 3 crosses both seams at x = 4 and x = 8.
		auto const spanning = world.addSectorWindow(0, 0, 3, 6, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		world.finishBuild();
		require(spanning.object != nullptr, "the three-Background spanning Window was not created");

		core::Vector2 wLo, wHi;
		spanning.object->getFullShape(wLo, wHi);
		auto const regions = backgroundApertureRegions(world, 1, wLo, wHi);
		require(regions.size() == 3, "a Window over three Backgrounds did not composite three regions");
		require(regions[0].background->getIndex() == firstIndex
				&& regions[1].background->getIndex() == secondIndex
				&& regions[2].background->getIndex() == thirdIndex,
			"the three regions are not in cell-grid order");
		require(regions[0].max.x == 4.0f && regions[1].min.x == 4.0f
				&& regions[1].max.x == 8.0f && regions[2].min.x == 8.0f,
			"the three regions do not meet exactly on the two cell boundaries");

		double covered{ 0.0 };
		for (auto const& region : regions) covered += rectOf(region).area();
		require(std::abs(covered - rectOf(wLo, wHi).area()) <= kAreaEpsilon,
			"the three-region composite does not cover the aperture exactly");

		// The control: a Window looking into a Room, not a Background, yields
		// no regions, so the caller keeps its single-sector path and the
		// generic back-layer tint.
		core::World rooms("Room-only aperture", 12, 2);
		while (rooms.getLayerCount() < 2) rooms.addLayer();
		rooms.addRoom("Front", 0, 0, 0, 6, 1);
		rooms.addRoom("Behind", 1, 0, 0, 6, 1);
		auto const intoRoom = rooms.addSectorWindow(0, 0, 1, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		rooms.finishBuild();
		require(intoRoom.object != nullptr, "the Window into a Room was not created");
		core::Vector2 rLo, rHi;
		intoRoom.object->getFullShape(rLo, rHi);
		require(backgroundApertureRegions(rooms, 1, rLo, rHi).empty(),
			"a Window into a Room yielded Background regions; the tint path would be overridden");
	}

	//
	// The same Depot paints the same picture every time it is built.
	//
	void theRenderSnapshotIsDeterministic()
	{
		core::World first("Render order depot", 16, 4);
		authorRenderOrderDepot(first);
		core::World second("Render order depot", 16, 4);
		authorRenderOrderDepot(second);

		require(snapshotDigest(first) == snapshotDigest(second),
			"two identical Depots painted different pictures");
	}
}

void runRenderOrderSmokeChecks()
{
	theDepotCarriesEveryClippedTransitBehindALayerOfLocations();
	aTransitBehindTheSelectionIsPaintedSolidThroughItsLocations();
	aTransitIsNotPaintedWhereTheSelectedLayerDoesNotOpen();
	aTransitIsNotPaintedFromAnyOtherLayer();
	onlyALandingLocationOpensOntoATransit();
	aTransitOnlyOpensOntoTheLayerInFrontOfIt();
	aLiftLandingDoorwayIsItsOwnThreshold();
	aShuttleOpensThroughItsOwnCarriageDoors();
	aStaircaseIsPaintedAcrossTheLocationsInView();
	aStaircaseUsesThePlainShaftSurface();
	theOverlayNeverLeaksSolidGeometryOrAgents();
	theSelectedLayerPaintsItselfWhole();
	theOverlayOutlinesTheWholeLayerBehind();
	theRenderPassOrderDrawsTheSelectedLayerFirst();
	aClearWindowShowsItsBackgroundsOwnColour();
	aBackgroundFillsSolidAndIsOutlinedOnlyByTheOverlay();
	aBackgroundBehindTheSelectionIsOutlinedWholeByTheOverlay();
	adjacentBackgroundsMeetWithoutASeam();
	aSelectedBackgroundStillTakesTheSelectionHighlight();
	aMultiBackgroundApertureCompositesEachBackgroundClipped();
	aMultiBackgroundApertureSpansEveryBackgroundAndSkipsNone();
	theRenderSnapshotIsDeterministic();
}
