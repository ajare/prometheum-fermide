#include "ObjectTileset.h"
#include "core/Defines.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <stdexcept>
#include <utility>
namespace {
ObjectTileset active;
ImTextureID textureId{};
}
ObjectTileset ObjectTileset::load(std::filesystem::path const& path)
{
    auto root = YAML::LoadFile(path.string());
    if (root["version"].as<int>() != 1) throw std::runtime_error("Unsupported object tileset version");
    ObjectTileset result;
    result.image = path.parent_path() / root["image"].as<std::string>();
    result.width = root["size"][0].as<int>();
    result.height = root["size"][1].as<int>();
    int const tw = root["tile_size"][0].as<int>();
    int const th = root["tile_size"][1].as<int>();
    if (tw != CORE_CELL_WIDTH_PIXELS || th != CORE_LEVEL_HEIGHT_PIXELS ||
        result.width <= 0 || result.height <= 0 || result.width % tw || result.height % th)
        throw std::runtime_error("Object atlas must contain whole 64x160 world-unit tiles");
    if (!root["sprites"].IsMap()) throw std::runtime_error("Missing object sprite definitions");
    for (auto const& item : root["sprites"]) {
        auto const v = item.second;
        if (!v["cell"].IsSequence() || v["cell"].size() != 2 ||
            !v["content"].IsSequence() || v["content"].size() != 4)
            throw std::runtime_error("Invalid object sprite rectangle");
        auto const c = v["content"];
        int const column = v["cell"][0].as<int>(), row = v["cell"][1].as<int>();
        SectorTileRegion r{c[0].as<int>(), c[1].as<int>(), c[2].as<int>(), c[3].as<int>()};
        if (column < 0 || row < 0 || column >= result.width / tw || row >= result.height / th ||
            r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 || r.width > tw || r.height > th ||
            r.x > tw - r.width || r.y > th - r.height)
            throw std::runtime_error("Object sprite is outside its cell");
        r.x += column * tw; r.y += row * th;
        if (!result.sprites.emplace(item.first.as<std::string>(), ObjectSprite{r, v["tintable"].as<bool>()}).second)
            throw std::runtime_error("Duplicate object sprite");
    }
    for (auto name : {"door", "button-enabled", "button-disabled", "window-clear", "window-frosted",
        "window-tinted", "agent", "marker", "platform-lift"})
        if (!result.sprites.count(name)) throw std::runtime_error("Missing object sprite: " + std::string(name));
    if (!result.sprites.at("agent").tintable) throw std::runtime_error("Agent sprite must support tinting");
    return result;
}
void setObjectTileset(ObjectTileset tileset, ImTextureID texture)
{
    active = std::move(tileset); textureId = texture;
}
void clearObjectTileset() { textureId = {}; active = {}; }
bool hasObjectTileset() { return textureId != ImTextureID{}; }
bool drawObjectSprite(char const* name, WorldDrawList* list, ImVec2 a, ImVec2 b,
    ImU32 tint, ImVec2 sourceMin, ImVec2 sourceMax)
{
    if (!textureId) return false;
    ImVec2 minimum{std::min(a.x, b.x), std::min(a.y, b.y)};
    ImVec2 maximum{std::max(a.x, b.x), std::max(a.y, b.y)};
    if (minimum.x == maximum.x || minimum.y == maximum.y) return true;
    auto const& sprite = active.sprites.at(name);
    auto const& r = sprite.region;
    auto uv = [&](ImVec2 fraction) {
        return ImVec2{(r.x + 0.5f + fraction.x * (r.width - 1)) / active.width,
            (r.y + 0.5f + fraction.y * (r.height - 1)) / active.height};
    };
    list->AddImage(WorldDrawList::Texture::ObjectAtlas, minimum, maximum,
        uv(sourceMin), uv(sourceMax), sprite.tintable ? tint : IM_COL32_WHITE);
    return true;
}
