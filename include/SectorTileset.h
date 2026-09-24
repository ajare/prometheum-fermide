#pragma once
#include <filesystem>
#include <map>
#include <string>
#include "imgui/imgui.h"

// Pixel regions are explicit: the source artwork is not a uniform grid.
struct SectorTileRegion { int x, y, width, height; };
struct SectorTileset
{
    std::filesystem::path image;
    int width{}, height{};
    std::map<std::string, SectorTileRegion> surfaces;
    std::map<std::string, SectorTileRegion> boundaries;
    static SectorTileset load(std::filesystem::path const& path);
};
void setSectorTileset(SectorTileset tileset, ImTextureID texture);
void clearSectorTileset();
bool drawSectorTileSurface(std::string const& kind, ImDrawList* drawList,
    ImVec2 minimum, ImVec2 maximum, ImU32 tint);
bool drawSectorTileBoundary(char const* kind, ImDrawList* drawList,
    ImVec2 minimum, ImVec2 maximum);
