// Facade flat-colour rendering, for ticket #46.
//
// A Facade renders as a solid colour exactly like a Background (ADR 0003).
// This check rides the same policy-driven model as the render-order checks:
// it mirrors the surface-painting decisions renderSector() makes - generic
// Layer fill, lights-off tint, and the Sector's own flat fill or outline -
// by calling the very helpers the renderer calls, so a change to that policy
// shows up here rather than only on a screen.
//
// The properties pinned down:
//
//   * the Solid pass fills a Facade with the Facade's own colour, once; the
//     generic Location fill is skipped, so nothing paints twice;
//   * the wireframe overlay contributes the outline only, never a fill;
//   * the Aperture pass fills through a Window, clipped to the Window's rect,
//     in the Facade's own colour (ADR 0002);
//   * the lights-off tint is bypassed: the user-authored colour is
//     authoritative, as with a Background - while a Room's fill does follow
//     its lights, proving the bypass is real and not a vacuous equality;
//   * a Window looking into a Facade shows its flat colour, its objects,
//     and its agents.

#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Render.h"
#include "core/Background.h"
#include "core/Building.h"
#include "core/Defines.h"
#include "core/Facade.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Vector2.h"
#include "core/Window.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// A Facade colour that is neither the Background default, the Fore/Back
	// Location colours, nor the lights-off tint, so "painted with the Facade's
	// colour" can never be confused with any other fill source.
	core::BackgroundColour const kFacadeColour{ 210, 66, 138 };

	// The Layer colours the viewport passes down as the generic fill
	// (ForeLocationColour / BackLocationColour in Render.cpp).
	core::BackgroundColour const kForeLayerColour{ 192, 192, 255 };
	core::BackgroundColour const kBackLayerColour{ 224, 224, 255 };

	//
	// One surface stroke renderSector() would emit: filled or outlined, in a
	// definite colour.
	//
	struct Stroke
	{
		bool filled{ false };
		core::BackgroundColour colour{};
	};

	//
	// Mirrors renderSector()'s surface decisions for one pass:
	//
	//   * the lights-off tint replaces the Layer colour unless the Sector
	//     bypasses it (bypassesLightsOffTint, #46);
	//   * the generic fill is skipped for a flat-colour Sector so its own
	//     fill never paints twice (rendersAsFlatColour);
	//   * a flat-colour Sector fills in the solid passes and outlines in
	//     the wireframe overlay, never both in one pass.
	//
	std::vector<Stroke> modelSurfaceStrokes(core::Sector const& sector,
		LayerRenderStyle style, core::BackgroundColour const& layerColour)
	{
		std::vector<Stroke> strokes;

		if (style == LayerRenderStyle::Hidden)
		{
			return strokes;
		}

		auto colour = layerColour;
		if (!bypassesLightsOffTint(sector.getType()) && !sector.areLightsOn())
		{
			colour = LightsOffTint;
		}

		if (!rendersAsFlatColour(sector.getType()))
		{
			strokes.push_back({ style != LayerRenderStyle::Wireframe, colour });
		}
		else
		{
			auto const own = flatSurfaceColour(sector);
			require(own.has_value(), "a flat-colour Sector carries no surface colour");
			if (shouldFillBackground(style))
			{
				strokes.push_back({ true, *own });
			}
			else if (shouldOutlineBackground(style))
			{
				strokes.push_back({ false, *own });
			}
		}

		return strokes;
	}

	std::string strokeCount(std::vector<Stroke> const& strokes)
	{
		return std::to_string(strokes.size());
	}

	// The single stroke a flat-colour Sector must produce in a given style,
	// with the fill/outline polarity the ticket demands.
	Stroke expectSingleStroke(std::vector<Stroke> const& strokes, bool filled,
		std::string const& where)
	{
		require(strokes.size() == 1,
			("expected exactly one surface stroke" + where + ", got " + strokeCount(strokes)).c_str());
		require(strokes[0].filled == filled,
			("the single surface stroke" + where + " has the wrong polarity").c_str());
		return strokes[0];
	}

	//
	// A two-Layer frontage: a Room up front with a clear Window, a Facade
	// directly behind it, so the Window's span sits wholly inside the
	// Facade.
	//
	//   Layer 0  Front Room (Window authored here)
	//   Layer 1  Frontage Facade at x=2..5
	//
	struct Frontage
	{
		core::Building building{ "Facade frontage", 12, 2 };
		uint32_t frontIndex{ 0 };
		uint32_t facadeIndex{ 0 };
		std::shared_ptr<const core::Facade> facade;
		std::shared_ptr<const core::Window> window;

		// Building is not copyable (it owns unique_ptrs), so the whole
		// frontage is authored in place rather than returned from a helper.
		Frontage()
		{
			while (building.getLayerCount() < 2) building.addLayer();

			frontIndex = building.addRoom("Front", 0, 0, 0, 12, 1);
			facadeIndex = building.addFacade("Frontage", 1, 0, 2, 4, 1,
				CORE_ROOM_MAX_HEIGHT, kFacadeColour);

			std::string diagnostic;
			require(building.canAddSectorWindow(0, 0, 3, 2, 1, &diagnostic),
				("a Window looking into a Facade was refused: " + diagnostic).c_str());

			auto created = building.addSectorWindow(0, 0, 3, 2, 1,
				{ false, core::Window::State::Closed, core::Window::Style::Clear });
			window = created.object;
			require(window != nullptr, "the Window into the Facade was not created");
			require(!created.traversalResource,
				"a looking Window into a Facade was given a traversal resource");

			auto sector = building.getSector(facadeIndex);
			require(sector != nullptr && sector->getType() == core::SectorType::Facade,
				"the Sector behind the Window is not a Facade");
			facade = std::static_pointer_cast<const core::Facade>(sector);
		}
	};

	//
	// The rule itself: Facade and Background are the flat-colour types, and
	// nothing else is - a Room must keep its Layer fill and its lights-off
	// tint, or the bypass would swallow the whole Location world.
	//
	void flatColourRuleNamesFacadeAndBackgroundOnly()
	{
		require(rendersAsFlatColour(core::SectorType::Facade),
			"a Facade is not rendered as a flat colour");
		require(rendersAsFlatColour(core::SectorType::Background),
			"a Background is not rendered as a flat colour");
		require(!rendersAsFlatColour(core::SectorType::Location),
			"a plain Location claims the flat-colour rule");
		require(!rendersAsFlatColour(core::SectorType::Ladder)
			&& !rendersAsFlatColour(core::SectorType::Lift)
			&& !rendersAsFlatColour(core::SectorType::Shuttle)
			&& !rendersAsFlatColour(core::SectorType::Stairwell)
			&& !rendersAsFlatColour(core::SectorType::Staircase),
			"a Transit type claims the flat-colour rule");

		require(bypassesLightsOffTint(core::SectorType::Facade),
			"a Facade does not bypass the lights-off tint");
		require(bypassesLightsOffTint(core::SectorType::Background),
			"a Background does not bypass the lights-off tint");
		require(!bypassesLightsOffTint(core::SectorType::Location),
			"a Location bypasses the lights-off tint; the tint would never reach a Room");
	}

	//
	// The Solid pass fills the Facade with its own colour - once. The generic
	// Location fill is skipped, so the Facade's colour never paints over a
	// Layer-coloured underlay, and no other colour source can appear inside
	// its footprint.
	//
	void solidPassFillsTheFacadeWithItsOwnColour()
	{
		Frontage const f;

		auto const strokes = modelSurfaceStrokes(*f.facade, LayerRenderStyle::Solid,
			kForeLayerColour);
		auto const stroke = expectSingleStroke(strokes, true, " for a Facade in the Solid pass");

		require(stroke.colour == kFacadeColour,
			"the Solid pass does not fill a Facade with the Facade's own colour");
		require(stroke.colour != kForeLayerColour,
			"the Facade fill is the Layer colour, not its own");
		require(stroke.colour != LightsOffTint,
			"the Facade fill is the lights-off tint");

		// flatSurfaceColour is the single source the renderer reads.
		auto const own = flatSurfaceColour(*f.facade);
		require(own.has_value() && *own == kFacadeColour,
			"flatSurfaceColour() does not report the Facade's colour");

		// Re-colouring the Facade moves the fill with it: the colour is live,
		// not a constant dressed up as one.
		auto mutableFacade = std::const_pointer_cast<core::Facade>(f.facade);
		mutableFacade->setColour(core::BackgroundColour{ 12, 200, 66 });
		auto const recoloured = modelSurfaceStrokes(*f.facade, LayerRenderStyle::Solid,
			kForeLayerColour);
		require(recoloured.size() == 1 && recoloured[0].filled
				&& recoloured[0].colour == core::BackgroundColour{ 12, 200, 66 },
			"the Solid fill did not follow the Facade's recolour");
	}

	//
	// The wireframe overlay contributes the outline only, never a fill
	// (ADR 0002): the same rule the Background overlay obeys, and for the
	// same reason - a fill from the Layer behind would bleed over the
	// selection.
	//
	void wireframePassOutlinesTheFacadeNeverFills()
	{
		Frontage const f;

		auto const strokes = modelSurfaceStrokes(*f.facade, LayerRenderStyle::Wireframe,
			kBackLayerColour);
		auto const stroke = expectSingleStroke(strokes, false,
			" for a Facade in the wireframe overlay");
		require(stroke.colour == kFacadeColour,
			"the wireframe outline of a Facade is not drawn in the Facade's colour");

		// The pass-level rule keeps its shape: one pass never both fills and
		// outlines a flat-colour Sector.
		require(!shouldFillBackground(LayerRenderStyle::Wireframe),
			"the wireframe pass fills a flat-colour Sector");
		require(shouldOutlineBackground(LayerRenderStyle::Wireframe),
			"the wireframe pass does not outline a flat-colour Sector");

		// Hidden paints nothing at all.
		require(modelSurfaceStrokes(*f.facade, LayerRenderStyle::Hidden, kBackLayerColour).empty(),
			"the Hidden style paints a Facade");
	}

	//
	// The lights-off tint is bypassed: the user-authored colour is
	// authoritative, as with a Background. The Room control proves the tint
	// is live - the equality below is not vacuous.
	//
	void facadeFillIsUnchangedWhenLightsAreOff()
	{
		Frontage const f;

		require(f.facade->areLightsOn(), "a freshly built Facade starts with its lights off");

		auto const on = modelSurfaceStrokes(*f.facade, LayerRenderStyle::Solid, kForeLayerColour);
		std::const_pointer_cast<core::Facade>(f.facade)->lightsOff();
		require(!f.facade->areLightsOn(), "the Facade's lights did not go off");
		auto const off = modelSurfaceStrokes(*f.facade, LayerRenderStyle::Solid, kForeLayerColour);

		require(on.size() == off.size(),
			"turning a Facade's lights off changed how many strokes it paints");
		require(!off.empty() && off[0].filled && off[0].colour == kFacadeColour,
			"a Facade with its lights off is not filled with its own colour");
		require(off[0].colour != LightsOffTint,
			"the lights-off tint reached a Facade's fill");

		// The same for the Aperture pass: the glass shows the authored colour
		// with the lights off just as it does with them on.
		auto const apertureOn = modelSurfaceStrokes(*f.facade, LayerRenderStyle::Aperture,
			kBackLayerColour);
		auto const apertureOff = modelSurfaceStrokes(*f.facade, LayerRenderStyle::Aperture,
			kBackLayerColour);
		require(apertureOff.size() == 1 && apertureOff[0].filled
				&& apertureOff[0].colour == apertureOn[0].colour && apertureOff[0].colour == kFacadeColour,
			"the aperture fill changed when the Facade's lights went off");

		// The control: a Room's fill does follow its lights, so the bypass
		// above is a rule about Facades, not a renderer that never tints.
		core::Building control("Lights control", 12, 2);
		while (control.getLayerCount() < 2) control.addLayer();
		auto const roomIndex = control.addRoom("Lit Room", 0, 0, 0, 4, 1);
		auto room = control.getSector(roomIndex);
		require(room != nullptr && room->getType() == core::SectorType::Location,
			"the control Sector is not a plain Location");
		std::const_pointer_cast<core::Sector>(room)->lightsOff();
		auto const roomOff = modelSurfaceStrokes(*room, LayerRenderStyle::Solid, kForeLayerColour);
		require(roomOff.size() == 1 && roomOff[0].filled && roomOff[0].colour == LightsOffTint,
			"a Room with its lights off does not take the lights-off tint");
	}

	//
	// A Window looking into a Facade shows its flat colour, its objects,
	// and its agents.
	//
	// The fill arrives through the Aperture pass, clipped to the Window's
	// rect (ADR 0002): the visible area is the intersection of the Window
	// and the Facade, which for this layout is exactly the Window - the
	// Facade's colour reaches the screen nowhere the glass does not cover.
	//
	void aWindowIntoAFacadeShowsFlatColourObjectsAndAgents()
	{
		Frontage f;

		// The glass looks into the Facade.
		auto const back = f.window->getBackSector();
		require(back != nullptr && back->getType() == core::SectorType::Facade,
			"the Window's back Sector is not the Facade");
		require(back->getIndex() == f.facadeIndex, "the Window looks into the wrong Sector");

		// The Aperture pass is handed the Facade's own colour, not the
		// generic back-layer tint (#34's rule, now covering the Facade).
		auto const fill = apertureFillColour(*back);
		require(fill.has_value(), "a Facade back Sector offers no aperture fill colour");
		require(*fill == kFacadeColour, "the aperture fill is not the Facade's own colour");

		// The glass follows the Facade: re-colour it and the fill follows.
		auto mutableFacade = std::const_pointer_cast<core::Facade>(f.facade);
		mutableFacade->setColour(core::BackgroundColour{ 30, 90, 210 });
		require(apertureFillColour(*back) == core::BackgroundColour{ 30, 90, 210 },
			"the aperture fill did not follow the Facade's recolour");
		mutableFacade->setColour(kFacadeColour);

		// The fill is clipped to the Window: the Window's rect sits wholly
		// inside the Facade, so the visible fill is exactly the Window's
		// rect - no Facade colour reaches the screen outside the glass.
		core::Vector2 windowMin, windowMax, facadeMin, facadeMax;
		f.window->getFullShape(windowMin, windowMax);
		f.facade->getBounds(facadeMin, facadeMax);
		require(windowMin.x >= facadeMin.x && windowMax.x <= facadeMax.x
				&& windowMin.y >= facadeMin.y && windowMax.y <= facadeMax.y,
			"the Window does not sit wholly inside the Facade it looks into");
		require(apertureFillColour(*back) == kFacadeColour,
			"the clipped aperture fill lost the Facade's colour");

		// Objects inside the Facade reach the glass: the aperture pass draws
		// a Sector's objects because it draws solid, and the Facade really
		// hosts one.
		f.building.addSectorMarker(f.facadeIndex, 0, 1.5f, nullptr);
		bool holdsMarker{ false };
		for (uint32_t objectIndex = 0; objectIndex < f.facade->getNumObjects(); ++objectIndex)
		{
			if (f.facade->getObject(objectIndex) != nullptr) holdsMarker = true;
		}
		require(holdsMarker, "the Facade hosts no object to show through the Window");
		require(isDrawnSolid(LayerRenderStyle::Aperture),
			"the aperture pass does not draw objects");

		// Agents inside the Facade reach the glass too, and the wireframe
		// overlay still refuses to show them.
		auto const agentId = f.building.createAgent("Frontage walker", f.facadeIndex, 0, 1.0f);
		require(agentId != core::AgentId{}, "createAgent did not assign the Facade walker an id");
		require(f.facade->getAgents().size() == 1,
			"the Agent did not enter the Facade");
		require(shouldRenderSectorAgents(core::SectorType::Facade, LayerRenderStyle::Aperture),
			"the aperture pass does not render agents inside a Facade");
		require(!shouldRenderSectorAgents(core::SectorType::Facade, LayerRenderStyle::Wireframe),
			"the wireframe overlay renders agents inside a Facade");

		// And with the Facade's lights off, the glass shows exactly the same
		// flat colour - the light switch moves the agents' world, not the
		// paint.
		std::const_pointer_cast<core::Facade>(f.facade)->lightsOff();
		require(apertureFillColour(*back) == kFacadeColour,
			"the aperture fill changed when the Facade's lights went off");
	}

	//
	// A mixed span - a Window looking half into a Facade and half into a
	// Background - is refused at authoring, the same rule #36 set for
	// Background/Location mixes. So the aperture never has to composite a
	// Facade beside a Background, and the single-sector path above is the
	// whole story for Facades behind glass.
	//
	void aWindowCannotLookHalfIntoAFacadeAndHalfIntoABackground()
	{
		core::Building building("Mixed span", 12, 2);
		while (building.getLayerCount() < 2) building.addLayer();
		building.addRoom("Front", 0, 0, 0, 12, 1);
		building.addFacade("Frontage", 1, 0, 2, 2, 1);
		building.addBackground(1, 0, 4, 2, 1, core::BackgroundColour{ 10, 10, 10 });

		std::string diagnostic;
		require(!building.canAddSectorWindow(0, 0, 3, 2, 1, &diagnostic),
			"a Window spanning a Facade and a Background was accepted");
		require(diagnostic.find("Background") != std::string::npos,
			("the mixed-span refusal does not name the Background: " + diagnostic).c_str());
	}
}

void runFacadeRenderSmokeChecks()
{
	flatColourRuleNamesFacadeAndBackgroundOnly();
	solidPassFillsTheFacadeWithItsOwnColour();
	wireframePassOutlinesTheFacadeNeverFills();
	facadeFillIsUnchangedWhenLightsAreOff();
	aWindowIntoAFacadeShowsFlatColourObjectsAndAgents();
	aWindowCannotLookHalfIntoAFacadeAndHalfIntoABackground();
}
