#pragma once
#include "SectorTileset.h"
struct ObjectSprite { SectorTileRegion region; bool tintable; };
struct ObjectTileset
{
    std::filesystem::path image;
    int width{}, height{};
    std::map<std::string, ObjectSprite> sprites;
    static ObjectTileset load(std::filesystem::path const& path);
};
void setObjectTileset(ObjectTileset tileset, ImTextureID texture);
void clearObjectTileset();
bool hasObjectTileset();
// Destination is the object's physical screen bounds, not its padded storage cell.
// Source fractions allow moving door leaves to be cropped without squashing.
bool drawObjectSprite(char const* name, ImDrawList* list, ImVec2 a, ImVec2 b,
    ImU32 tint = IM_COL32_WHITE, ImVec2 sourceMin = {0, 0}, ImVec2 sourceMax = {1, 1});
