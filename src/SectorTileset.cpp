#include "SectorTileset.h"
#include "core/Defines.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {
SectorTileset active;
ImTextureID textureId{};
void image(ImDrawList* list, SectorTileRegion const& r, ImVec2 a, ImVec2 b,
    float fractionX, float fractionY, ImU32 tint)
{
    // Half-texel inset avoids sampling adjacent draft artwork with linear filtering.
    ImVec2 uv0{(r.x + 0.5f) / active.width, (r.y + 0.5f) / active.height};
    ImVec2 uv1{(r.x + 0.5f + (r.width - 1) * fractionX) / active.width,
        (r.y + 0.5f + (r.height - 1) * fractionY) / active.height};
    list->AddImage(textureId, a, b, uv0, uv1, tint);
}
}

SectorTileset SectorTileset::load(std::filesystem::path const& path)
{
    auto root = YAML::LoadFile(path.string());
    if (root["version"].as<int>() != 1)
        throw std::runtime_error("Unsupported sector tileset version");
    SectorTileset result;
    result.image = path.parent_path() / root["image"].as<std::string>();
    result.width = root["size"][0].as<int>();
    result.height = root["size"][1].as<int>();
    if (result.width <= 0 || result.height <= 0)
        throw std::runtime_error("Invalid sector atlas dimensions");
    auto read = [&](char const* key, auto& destination) {
        if (!root[key].IsMap()) throw std::runtime_error("Missing tileset region map");
        for (auto const& entry : root[key]) {
            auto const v = entry.second;
            if (!v.IsSequence() || v.size() != 4)
                throw std::runtime_error("Tileset regions require [x, y, width, height]");
            SectorTileRegion r{v[0].as<int>(), v[1].as<int>(), v[2].as<int>(), v[3].as<int>()};
            if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 ||
                r.width > result.width || r.height > result.height ||
                r.x > result.width - r.width || r.y > result.height - r.height)
                throw std::runtime_error("Sector atlas region is out of bounds");
            destination.emplace(entry.first.as<std::string>(), r);
        }
    };
    read("surfaces", result.surfaces);
    read("boundaries", result.boundaries);
    for (auto name : {"corridor", "room", "ladder", "lift", "shuttle", "stairwell", "staircase"})
        if (!result.surfaces.count(name)) throw std::runtime_error("Missing sector surface: " + std::string(name));
    for (auto name : {"floor", "ceiling", "left", "right"})
        if (!result.boundaries.count(name)) throw std::runtime_error("Missing sector boundary: " + std::string(name));
    return result;
}

void setSectorTileset(SectorTileset tileset, ImTextureID texture)
{
    active = std::move(tileset);
    textureId = texture;
}
void clearSectorTileset() { textureId = {}; active = {}; }

bool drawSectorTileSurface(std::string const& kind, ImDrawList* list,
    ImVec2 minimum, ImVec2 maximum, ImU32 tint)
{
    if (!textureId) return false;
    auto const& region = active.surfaces.at(kind);
    // One world unit is 64 screen pixels wide and 160 high. Crop partial
    // levels instead of stretching a complete tile to a Corridor's height.
    auto const clipMin = list->GetClipRectMin();
    auto const clipMax = list->GetClipRectMax();
    int const x0 = std::max(0, static_cast<int>((clipMin.x - minimum.x) / CORE_CELL_WIDTH_PIXELS));
    int const y0 = std::max(0, static_cast<int>((clipMin.y - minimum.y) / CORE_LEVEL_HEIGHT_PIXELS));
    for (float y = minimum.y + y0 * CORE_LEVEL_HEIGHT_PIXELS; y < std::min(maximum.y, clipMax.y); y += CORE_LEVEL_HEIGHT_PIXELS)
        for (float x = minimum.x + x0 * CORE_CELL_WIDTH_PIXELS; x < std::min(maximum.x, clipMax.x); x += CORE_CELL_WIDTH_PIXELS) {
            ImVec2 end{std::min(x + CORE_CELL_WIDTH_PIXELS, maximum.x), std::min(y + CORE_LEVEL_HEIGHT_PIXELS, maximum.y)};
            image(list, region, {x, y}, end, (end.x - x) / CORE_CELL_WIDTH_PIXELS,
                (end.y - y) / CORE_LEVEL_HEIGHT_PIXELS, tint);
        }
    return true;
}

bool drawSectorTileBoundary(char const* kind, ImDrawList* list, ImVec2 a, ImVec2 b)
{
    if (!textureId) return false;
    image(list, active.boundaries.at(kind), a, b, 1, 1, IM_COL32_WHITE);
    return true;
}
