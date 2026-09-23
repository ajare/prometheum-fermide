#pragma once

#include <functional>
#include <memory>
#include <string>

namespace core
{
	class Building;
	class SectorObject;
}

// Extracted Marker editor used by the graphical editor and CPU-side ImGui
// verification. The optional reporter lets the application retain its normal
// error presentation without coupling this panel to UI.cpp globals.
using MarkerPanelErrorReporter = std::function<void(std::string const&)>;

void renderMarkerEditorPanel(
	std::shared_ptr<core::Building> const& building,
	std::shared_ptr<const core::SectorObject> const& object,
	MarkerPanelErrorReporter const& reportError = {});
