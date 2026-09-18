// Tickets #16 and #25: rendering order for multi-layer Buildings.
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
// renderBuilding() does, and drives them through the very policy helpers the
// renderer calls - renderPasses, shouldClipTransitToApertures, transitApertures
// and shouldRenderSectorAgents - so a change to that policy shows up here rather
// than only on a screen.

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
#include "core/Building.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Location.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/SectorType.h"
#include "core/Transit.h"
#include "core/Vector2.h"

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

	std::vector<std::shared_ptr<const core::Sector>> locationsOn(
		core::Building const& building, uint32_t layer)
	{
		std::vector<std::shared_ptr<const core::Sector>> locations;
		for (auto const& sector : building.getSectors(layer))
			if (std::dynamic_pointer_cast<const core::Location>(sector)) locations.push_back(sector);
		return locations;
	}

	//
	// Paints the whole Building for one selected Layer, exactly as the renderer
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
	LayerView snapshotView(core::Building const& building, uint32_t viewLayer,
		std::vector<std::shared_ptr<const core::Sector>> const& viewLocations, bool overlayEnabled)
	{
		LayerView view;
		view.viewLayer = viewLayer;
		view.overlayEnabled = overlayEnabled;
		for (auto const& location : viewLocations)
		{
			view.selectedLocations.push_back(rectOf(*location));
		}

		auto const layerCount = building.getLayerCount();

		std::map<uint32_t, PaintedSector> painted;
		for (uint32_t index = 0; index < building.getNumSectors(); ++index)
		{
			auto const sector = building.getSector(index);
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

				if (pass.style == LayerRenderStyle::Aperture)
				{
					// Only a Transit is drawn through an aperture by this pass; a
					// Location reaches the screen through a threshold instead.
					if (isLocation) continue;

					for (auto const& aperture : transitApertures(building.getSector(index),
						viewLayer, viewLocations))
					{
						auto const piece = intersect(sector.footprint, rectOf(aperture));
						if (piece.area() > 0.0)
						{
							sector.strokes.push_back({ piece, true });
						}
					}
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

	LayerView snapshotView(core::Building const& building, uint32_t viewLayer, bool overlayEnabled = true)
	{
		return snapshotView(building, viewLayer, locationsOn(building, viewLayer), overlayEnabled);
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
	void authorRenderOrderDepot(core::Building& building)
	{
		while (building.getLayerCount() < 4) building.addLayer();

		building.addRoom("Front Hall", 0, 0, 0, 14, 1);
		building.addRoom("Front Shop", 0, 1, 0, 14, 1);
		building.addRoom("Front Store", 0, 2, 0, 14, 1);
		building.addRoom("Front Yard", 0, 3, 0, 14, 1);

		core::Building::CreateLadderOptions ladderOptions{ 2, false, true };
		building.addLadder(1, 0, 10, ladderOptions);

		building.addRoom("Back Loft", 1, 2, 0, 8, 1);
		building.addRoom("Back Gallery", 1, 3, 0, 8, 1);

		building.addStairwell(1, 2, 8, 2, CORE_SIDE_RIGHT);

		core::Building::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.stopOffsets = { 0, 1 };
		building.addLift(2, 2, 4, liftOptions);

		building.addRoom("Deep Store", 2, 0, 0, 14, 1);
		building.addRoom("Deep Yard", 2, 1, 0, 14, 1);

		core::Building::CreateShuttleOptions shuttleOptions{ 2, 3, { 0, 7 }, 0 };
		building.addShuttle(3, 0, 1, 15, shuttleOptions);

		building.finishBuild();
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

	std::shared_ptr<const core::Sector> sectorByName(core::Building const& building,
		std::string_view name)
	{
		for (uint32_t index = 0; index < building.getNumSectors(); ++index)
			if (building.getSector(index)->getName() == name) return building.getSector(index);
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
		core::Building building("Render order depot", 16, 4);
		authorRenderOrderDepot(building);

		require(building.getLayerCount() == 4, "the Depot is not four Layers deep");
		require(building.isTraversalTopologyValid(),
			("the Depot's traversal topology is invalid: " + building.getTopologyDiagnostic()).c_str());

		for (auto const& want : depotTransits())
		{
			auto const transit = std::dynamic_pointer_cast<const core::Transit>(
				sectorByName(building, want.name));
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
		auto const ladder = rectOf(*sectorByName(building, "Ladder"));
		auto const shop = rectOf(*sectorByName(building, "Front Shop"));
		Rect const aboveTheCeiling{ ladder.minX, shop.maxY, ladder.maxX, ladder.maxY };
		require(aboveTheCeiling.area() > kAreaEpsilon,
			("the Depot no longer leaves the Ladder's shaft above its landing ceiling: "
				+ describeRect(aboveTheCeiling)).c_str());

		// The Shuttle runs past the right-hand wall of the Room it is seen through.
		auto const shuttle = rectOf(*sectorByName(building, "Shuttle"));
		auto const store = rectOf(*sectorByName(building, "Deep Store"));
		Rect const pastTheWall{ store.maxX, shuttle.minY, shuttle.maxX, shuttle.maxY };
		require(pastTheWall.area() > kAreaEpsilon,
			("the Depot no longer runs the Shuttle past its landing Location: "
				+ describeRect(pastTheWall)).c_str());

		// And the Depot's front Layer is not a solid wall of Sectors: ground is
		// left empty at the right-hand end, which is what makes "and not otherwise"
		// a question with something to answer.
		double frontRight{ 0.0 };
		for (auto const& location : locationsOn(building, 0))
		{
			frontRight = std::max(frontRight, rectOf(*location).maxX);
		}
		require(frontRight + kAreaEpsilon < static_cast<double>(building.getCellsWide()),
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
		core::Building building("Render order depot", 16, 4);
		authorRenderOrderDepot(building);

		for (auto const& want : depotTransits())
		{
			auto const viewLayer = want.layer - 1;

			for (auto const overlay : { false, true })
			{
				auto const view = snapshotView(building, viewLayer,
					locationsOn(building, viewLayer), overlay);
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
		core::Building building("Render order depot", 16, 4);
		authorRenderOrderDepot(building);

		auto const view = snapshotView(building, 0);
		auto const& ladder = paintedSector(view, "Ladder");
		auto const& shop = rectOf(*sectorByName(building, "Front Shop"));
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

		auto const deepView = snapshotView(building, 2);
		auto const& shuttle = paintedSector(deepView, "Shuttle");
		auto const& store = rectOf(*sectorByName(building, "Deep Store"));
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
		for (uint32_t viewLayer = 0; viewLayer + 1 < building.getLayerCount(); ++viewLayer)
		{
			auto const layerView = snapshotView(building, viewLayer);
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
		core::Building building("Render order depot", 16, 4);
		authorRenderOrderDepot(building);

		constexpr uint32_t viewLayer{ 1 };	// the Depot's Lift sits on Layer 2
		auto const transit = sectorByName(building, "Lift");
		require(transit != nullptr, "the Depot has no Lift");

		auto const apertures = transitApertures(transit, viewLayer, locationsOn(building, viewLayer));
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
	// Only the selected Layer and the one directly behind it reach the screen. A
	// Transit two Layers back, or one in front of the selection, is not painted at
	// all, however much its footprint would overlap what the viewer can see.
	//
	void aTransitIsNotPaintedFromAnyOtherLayer()
	{
		core::Building building("Render order depot", 16, 4);
		authorRenderOrderDepot(building);

		auto const layerCount = building.getLayerCount();
		for (uint32_t viewLayer = 0; viewLayer < layerCount; ++viewLayer)
		{
			for (auto const overlay : { false, true })
			{
				auto const view = snapshotView(building, viewLayer,
					locationsOn(building, viewLayer), overlay);
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
		core::Building building("Render order depot", 16, 4);
		authorRenderOrderDepot(building);

		for (auto const& want : depotTransits())
		{
			auto const viewLayer = want.layer - 1;
			auto const transit = sectorByName(building, want.name);
			auto const apertures = transitApertures(transit, viewLayer, locationsOn(building, viewLayer));
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
			for (auto const& location : locationsOn(building, viewLayer))
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
			for (auto const& other : building.getSectors(viewLayer))
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
		core::Building building("Render order depot", 16, 4);
		authorRenderOrderDepot(building);

		for (auto const& want : depotTransits())
		{
			auto const transit = sectorByName(building, want.name);
			for (uint32_t viewLayer = 0; viewLayer < building.getLayerCount(); ++viewLayer)
			{
				auto const apertures = transitApertures(transit, viewLayer,
					locationsOn(building, viewLayer));
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
		core::Building building("Staircase render order", 12, 3);
		while (building.getLayerCount() < 2) building.addLayer();
		building.addCorridor(0, 0, 6);
		building.addCorridor(1, 0, 7);
		auto const room = building.addRoom("Upper room", 0, 1, 7, 3, 1);
		building.removeLocationWall(room, 0, CORE_SIDE_LEFT);
		building.addStaircase(1, 0, 5,
			core::Building::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.0f });
		building.finishBuild();
		require(building.isTraversalTopologyValid(),
			("the Staircase Building's traversal topology is invalid: "
				+ building.getTopologyDiagnostic()).c_str());

		for (auto const overlay : { false, true })
		{
			auto const view = snapshotView(building, 0, locationsOn(building, 0), overlay);
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
		auto const emptyView = snapshotView(building, 0, none, true);
		require(paintedSector(emptyView, "Staircase").solidArea() <= kAreaEpsilon,
			"the Staircase is painted solid with no Location of the selected Layer in view");

		// With only the lower Corridor in view, none of it reaches above that
		// Corridor's ceiling.
		std::vector<std::shared_ptr<const core::Sector>> lowerOnly;
		Rect lowerCeiling{};
		for (auto const& sector : building.getSectors(0))
		{
			if (sector->getCellY() != 0) continue;
			lowerOnly.push_back(sector);
			lowerCeiling = rectOf(*sector);
		}
		require(!lowerOnly.empty(), "the Staircase Building has no lower Corridor");
		auto const lowerView = snapshotView(building, 0, lowerOnly, true);
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
		core::Building building("Render order depot", 16, 4);
		authorRenderOrderDepot(building);

		for (uint32_t viewLayer = 0; viewLayer < building.getLayerCount(); ++viewLayer)
		{
			auto const without = snapshotView(building, viewLayer,
				locationsOn(building, viewLayer), false);
			auto const with = snapshotView(building, viewLayer,
				locationsOn(building, viewLayer), true);

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
		core::Building building("Render order depot", 16, 4);
		authorRenderOrderDepot(building);

		for (uint32_t viewLayer = 0; viewLayer < building.getLayerCount(); ++viewLayer)
		{
			auto const view = snapshotView(building, viewLayer);
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
		core::Building building("Render order depot", 16, 4);
		authorRenderOrderDepot(building);

		for (uint32_t viewLayer = 0; viewLayer + 1 < building.getLayerCount(); ++viewLayer)
		{
			auto const without = snapshotView(building, viewLayer,
				locationsOn(building, viewLayer), false);
			auto const with = snapshotView(building, viewLayer,
				locationsOn(building, viewLayer), true);

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
	// renderBuilding() drives, so this is the renderer's own order.
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

	uint64_t snapshotDigest(core::Building const& building)
	{
		uint64_t digest{ 1469598103934665603ULL };
		for (uint32_t viewLayer = 0; viewLayer < building.getLayerCount(); ++viewLayer)
		{
			for (uint32_t overlay = 0; overlay < 2; ++overlay)
			{
				digestValue(digest, viewLayer);
				digestValue(digest, overlay);
				auto const view = snapshotView(building, viewLayer,
					locationsOn(building, viewLayer), overlay != 0);
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
	// The same Depot paints the same picture every time it is built.
	//
	void theRenderSnapshotIsDeterministic()
	{
		core::Building first("Render order depot", 16, 4);
		authorRenderOrderDepot(first);
		core::Building second("Render order depot", 16, 4);
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
	aStaircaseIsPaintedAcrossTheLocationsInView();
	theOverlayNeverLeaksSolidGeometryOrAgents();
	theSelectedLayerPaintsItselfWhole();
	theOverlayOutlinesTheWholeLayerBehind();
	theRenderPassOrderDrawsTheSelectedLayerFirst();
	theRenderSnapshotIsDeterministic();
}
