#pragma once

#include <cstdint>
#include <filesystem>

#include "imgui/imgui.h"

class WorldDrawList;

// Creates the process-wide MPP and Willpower resource stack against the
// already-current OpenGL context, then loads Resources.yaml and the mandatory
// viewport ImageSets.
void createWorldRenderSystem(std::filesystem::path const& resourceDirectory,
	std::size_t drawableWidth, std::size_t drawableHeight);
void resizeWorldRenderSystem(std::size_t drawableWidth, std::size_t drawableHeight);
void destroyWorldRenderSystem();

// Renders one CPU command stream into the MPP-owned offscreen target and
// returns its OpenGL texture name for composition by ImGui.
std::uint32_t renderWorldCommands(WorldDrawList const& commands,
	ImVec2 canvasPosition, ImVec2 canvasSize);
