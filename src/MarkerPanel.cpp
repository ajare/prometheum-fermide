#include "MarkerPanel.h"

#include <array>
#include <cstring>
#include <utility>

#include "DocumentEdit.h"
#include "core/Building.h"
#include "core/Log.h"
#include "core/Marker.h"
#include "core/MarkerSectorObject.h"
#include "imgui/imgui.h"

void renderMarkerEditorPanel(
	std::shared_ptr<core::Building> const& building,
	std::shared_ptr<const core::SectorObject> const& object,
	MarkerPanelErrorReporter const& reportError)
{
	if (!building || !object
		|| object->getObjectType() != core::SectorObjectType::Marker)
	{
		ImGui::TextDisabled("No Marker selected.");
		return;
	}

	auto marker = std::static_pointer_cast<const core::MarkerSectorObject>(
		object)->getMarker();
	static core::MarkerId editingId{};
	static std::array<char, core::Marker::MaxNameBytes + 1> name{};
	if (editingId != marker->getId())
	{
		editingId = marker->getId();
		std::strncpy(name.data(), marker->getName().c_str(), name.size() - 1);
		name.back() = '\0';
	}

	ImGui::TextUnformatted("Marker");
	ImGui::SetNextItemWidth(-1.0f);
	bool const submitted = ImGui::InputText("Name", name.data(), name.size(),
		ImGuiInputTextFlags_EnterReturnsTrue);
	if (submitted || ImGui::IsItemDeactivatedAfterEdit())
	{
		auto const trimmed = core::Marker::trimName(name.data());
		if (trimmed != marker->getName())
		{
			auto undo = captureDocumentSnapshot(building);
			std::string diagnostic;
			if (building->renameMarker(marker->getId(), name.data(), &diagnostic))
				commitDocumentEdit(std::move(undo));
			else if (reportError)
				reportError(diagnostic);
			else
				core::addLogMessage("Marker editor", 0,
					core::LogLevel::Warning, diagnostic);
		}
		std::strncpy(name.data(), marker->getName().c_str(), name.size() - 1);
		name.back() = '\0';
	}
	auto position = marker->getPosition();
	position.x += marker->getOffset();
	ImGui::Text("Position: %.2f, %.2f",
		static_cast<double>(position.x), static_cast<double>(position.y));
}
