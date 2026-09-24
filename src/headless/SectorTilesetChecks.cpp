#include "SectorTileset.h"
#include "ObjectTileset.h"
#include "imgui/imgui_internal.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    ImGui::CreateContext();
    try {
        auto tileset = SectorTileset::load(argv[1]);
        if (!std::filesystem::is_regular_file(tileset.image))
            throw std::runtime_error("Atlas image missing");
        setSectorTileset(tileset, (ImTextureID)(intptr_t)123);
        ImDrawList list(ImGui::GetDrawListSharedData());
        list._ResetForNewFrame();
        list.PushClipRect({0, 0}, {1000, 1000});
        for (auto const& [name, region] : tileset.surfaces) {
            (void)region;
            int const start = list.VtxBuffer.Size;
            if (!drawSectorTileSurface(name, &list, {0, 0}, {128, 240}, IM_COL32_WHITE))
                throw std::runtime_error("Surface was not textured");
            if (list.VtxBuffer.Size - start != 16)
                throw std::runtime_error("Expected four quads including partial upper level");
            for (int i = start; i < list.VtxBuffer.Size; ++i) {
                auto const& v = list.VtxBuffer[i];
                if (v.pos.x < 0 || v.pos.x > 128 || v.pos.y < 0 || v.pos.y > 240 ||
                    v.uv.x < 0 || v.uv.x > 1 || v.uv.y < 0 || v.uv.y > 1)
                    throw std::runtime_error("Invalid tile position or UV");
            }
        }
        for (auto name : {"left", "right", "floor", "ceiling"})
            if (!drawSectorTileBoundary(name, &list, {0, 0}, {2, 160}))
                throw std::runtime_error("Missing textured boundary");
        clearSectorTileset();
        if (drawSectorTileSurface("room", &list, {0, 0}, {64, 160}, IM_COL32_WHITE))
            throw std::runtime_error("Texture remained active after cleanup");
        auto objects = ObjectTileset::load(std::filesystem::path(argv[1]).parent_path() / "objects.tileset.yaml");
        if (!std::filesystem::is_regular_file(objects.image))
            throw std::runtime_error("Object atlas missing");
        setObjectTileset(objects, (ImTextureID)(intptr_t)456);
        ImU32 const overrideColour = IM_COL32(21, 173, 91, 255);
        for (auto const& [name, sprite] : objects.sprites) {
            int const start = list.VtxBuffer.Size;
            if (!drawObjectSprite(name.c_str(), &list, {10, 92}, {36, 20}, overrideColour))
                throw std::runtime_error("Object was not textured");
            if (list.VtxBuffer.Size != start + 4)
                throw std::runtime_error("Expected one object quad");
            for (int i = start; i < list.VtxBuffer.Size; ++i) {
                auto const& vertex = list.VtxBuffer[i];
                if (vertex.col != (sprite.tintable ? overrideColour : IM_COL32_WHITE))
                    throw std::runtime_error("Object colour override incorrect");
                if (vertex.pos.x < 10 || vertex.pos.x > 36 || vertex.pos.y < 20 || vertex.pos.y > 92)
                    throw std::runtime_error("Object sizing incorrect");
            }
        }
        int const start = list.VtxBuffer.Size;
        drawObjectSprite("door", &list, {0, 0}, {26, 80}, IM_COL32_WHITE, {0.5f, 0}, {1, 1});
        auto const& doorRegion = objects.sprites.at("door").region;
        float const expectedU = (doorRegion.x + 0.5f + (doorRegion.width - 1) * 0.5f) / objects.width;
        if (list.VtxBuffer[start].uv.x != expectedU)
            throw std::runtime_error("Door crop does not preserve sliding UVs");
        int const beforeEmpty = list.VtxBuffer.Size;
        drawObjectSprite("door", &list, {0, 0}, {0, 80});
        if (list.VtxBuffer.Size != beforeEmpty)
            throw std::runtime_error("Fully open door emitted geometry");
        clearObjectTileset();
        if (drawObjectSprite("agent", &list, {0, 0}, {26, 72}))
            throw std::runtime_error("Object texture active after cleanup");
        list.PopClipRect();
    } catch (std::exception const& e) {
        clearObjectTileset();
        clearSectorTileset();
        ImGui::DestroyContext();
        std::cerr << e.what() << '\n';
        return 1;
    }
    ImGui::DestroyContext();
    return 0;
}
