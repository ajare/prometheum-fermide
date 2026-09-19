// Application-owned globals for the headless link of Render.cpp (#49).
//
// The draw-order check exercises the real renderer, so the globals Main.cpp
// owns in the GUI build need headless counterparts. Nothing here touches a
// window, a dialog, or the GPU: the stubs sit at their defaults, which is
// exactly how the renderer treats "nothing selected, no icon font".

#include <memory>

#include "imgui/imgui.h"

#include "core/Agent.h"
#include "core/Sector.h"
#include "core/SectorObject.h"
#include "core/Vertex.h"
#include "core/Vector2.h"
#include "UISettings.h"

UISettings gUISettings;

core::Agent* gHoveredAgent{ nullptr };
core::Agent* gSelectedAgent{ nullptr };

std::shared_ptr<const core::Vertex> gSelectedVertex;
std::shared_ptr<const core::Sector> gSelectedSector;
std::shared_ptr<const core::SectorObject> gHoveredSectorObject;
std::shared_ptr<const core::SectorObject> gSelectedSectorObject;

ImFont* gAgentIconFont{ nullptr };

// The real implementation reads the mouse from the live viewport. Headless
// there is no mouse; the renderer only consults this when
// gUISettings.highlightNearestVertex is on, which the stub leaves off.
core::Vector2 getMouseWorldPosition()
{
	return core::Vector2{ 0.0f, 0.0f };
}
