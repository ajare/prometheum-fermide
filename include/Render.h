#pragma once

#include <cstdint>
#include <memory>

#include "core/Building.h"
#include "core/Graph.h"
#include "core/SectorType.h"


#define RENDER_VERTEX_SIZE						5
#define RENDER_INTER_LAYER_EDGE_SIZE			11

//
// How a Sector is drawn while a given Layer is selected in the viewport.
//
// The selected Layer is drawn solid and the Layer directly behind it is drawn as a
// wireframe overlay. Every other Layer is hidden: nothing in front of the
// selection, and nothing more than one Layer behind it.
//
enum class LayerRenderStyle
{
	Hidden,		// The Layer is not drawn at all.
	Solid,		// The selected Layer.
	Wireframe,	// The Layer directly behind the selected Layer.
	Aperture	// The Layer behind, seen through an aperture in the selected Layer.
};

inline LayerRenderStyle layerRenderStyle(uint32_t layer, uint32_t viewLayer, uint32_t layerCount)
{
	if (layer >= layerCount || viewLayer >= layerCount)
	{
		return LayerRenderStyle::Hidden;
	}

	if (layer == viewLayer)
	{
		return LayerRenderStyle::Solid;
	}

	if (viewLayer + 1 < layerCount && layer == core::layerBehind(viewLayer))
	{
		return LayerRenderStyle::Wireframe;
	}

	return LayerRenderStyle::Hidden;
}

inline bool isLayerDrawn(uint32_t layer, uint32_t viewLayer, uint32_t layerCount)
{
	return layerRenderStyle(layer, viewLayer, layerCount) != LayerRenderStyle::Hidden;
}

// Filled rather than outlined. Both the selected Layer and the Layer seen through
// one of its apertures are drawn solid; only the wireframe overlay is not.
inline bool isDrawnSolid(LayerRenderStyle style)
{
	return style == LayerRenderStyle::Solid || style == LayerRenderStyle::Aperture;
}

// Transit geometry is drawn by the selected Layer's passes. The wireframe overlay
// contributes outlines, never the Transit itself, which would otherwise paint over
// the selected Layer.
inline bool shouldRenderLadderGeometry(LayerRenderStyle style)
{
	return isDrawnSolid(style);
}

inline bool shouldRenderStairwellGeometry(LayerRenderStyle style)
{
	return isDrawnSolid(style);
}

inline bool shouldRenderForeContentAfterTransit(core::SectorType transitType)
{
	return transitType == core::SectorType::Ladder;
}

inline bool shouldRenderLadderGeometryAfterSectorContents()
{
	return false;
}

inline bool shouldRenderStaircaseAfterSector(core::SectorType sectorType)
{
	// The selected Layer's Transit pass runs after Locations. Both Corridors and
	// Rooms are Location sectors and may expose part of a Staircase.
	return sectorType == core::SectorType::Location;
}

// Occupants follow the same rule as their Sector's geometry. The wireframe overlay
// may outline its Sectors, but must not expose the Agents inside them.
inline bool shouldRenderSectorAgents(core::SectorType /* sectorType */, LayerRenderStyle style)
{
	return isDrawnSolid(style);
}

void renderGraph(std::shared_ptr<const core::Graph> graph, std::shared_ptr<const core::Building> building);

void renderBuilding(std::shared_ptr<const core::Building> building);
