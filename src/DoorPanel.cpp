// The Selection panel's Door branch; see include/DoorPanel.h. Extracted from
// UI.cpp for ticket #99 so the headless smoke checks render the real panel.

#include <memory>
#include <stdexcept>
#include <string>

#include "DoorPanel.h"

#include "imgui/imgui.h"

#include "core/Building.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Exceptions.h"
#include "core/Log.h"
#include "core/Sector.h"
#include "core/SectorObject.h"

#include "DocumentEdit.h"
#include "UISettings.h"

using namespace std;

extern UISettings gUISettings;

namespace
{
	char const* doorOpenStyleLabel(core::Door::OpenStyle style)
	{
		switch (style)
		{
		case core::Door::OpenStyle::OpenUp: return "Open Up";
		case core::Door::OpenStyle::OpenLeft: return "Open Left";
		case core::Door::OpenStyle::OpenRight: return "Open Right";
		case core::Door::OpenStyle::OpenApart: return "Open Apart";
		}
		return "Open Up";
	}
}

// Extracted verbatim from UI.cpp (ticket #99), with that ticket's fix: the
// opening-style selector's disabled scope is closed immediately after the
// selector, before the Add Door Button opens its own scope. The two scopes
// must nest as siblings, never as an unbalanced outer pair.
void renderDoorPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
{
	auto doorObject = static_pointer_cast<const core::DoorSectorObject>(object);
	auto door = doorObject->getDoor();
	auto position = door->getPosition();

	ImGui::TextUnformatted("Door");
	ImGui::Text("Position: %.2f, %.2f", position.x, position.y);
	ImGui::Text("Width: %u cell%s", door->getCellsWide(),
		door->getCellsWide() == 1 ? "" : "s");
	ImGui::Text("Height: %u deck%s", door->getDecksHigh(),
		door->getDecksHigh() == 1 ? "" : "s");

	float pct = door->getOpenPercentage() * 100;

	// State
	switch (door->getState())
	{
	case core::OpenableObject::State::Open:
		ImGui::Text("Open (%3.2f%% open)", pct);
		break;

	case core::OpenableObject::State::Opening:
		ImGui::Text("Opening (%3.2f%% open)", pct);
		break;

	case core::OpenableObject::State::Closed:
		ImGui::Text("Closed (%3.2f%% open)", pct);
		break;

	case core::OpenableObject::State::Closing:
		ImGui::Text("Closing (%3.2f%% open)", pct);
		break;
	}

	// Open/close time
	ImGui::Text("Open/close time: %3.2fs", door->getOpenCloseTime());
	ImGui::Text("Open wait time: %3.2fs", door->getOpenWaitTime());

	// Sectors
	ImGui::Text("From: %s", door->getFrontSector()->getDescription().c_str());
	ImGui::Text("To: %s", door->getBackSector()->getDescription().c_str());

	uint32_t liftSector, stopIndex, carriageIndex, doorIndex;
	bool const liftOwned = building->isLiftOwnedDoor(object, &liftSector, &stopIndex);
	bool const shuttleOwned = building->isShuttleOwnedDoor(object, &liftSector, &stopIndex,
		&carriageIndex, &doorIndex);
	if (liftOwned || shuttleOwned)
	{
		ImGui::Separator();
		ImGui::Text("Owned by %s", liftOwned ? "Lift" : "Shuttle");
		ImGui::Text("%s sector: %u", liftOwned ? "Lift" : "Shuttle", liftSector);
		if (liftOwned) ImGui::Text("Stop: %u (floor %u)", stopIndex, object->getCellY());
		else ImGui::Text("Stop: %u, carriage: %u, door: %u", stopIndex, carriageIndex, doorIndex);
		ImGui::TextDisabled("Landing geometry and controls are managed by the transport.");
		ImGui::TextDisabled(liftOwned
			? "The opening style is authored per stop and can be changed here."
			: "The opening style is authored per Door and can be changed here.");
	}

	ImGui::BeginDisabled(!building->isSimulationPaused());
	// The combo index is the position in this list, not the enum value, so the
	// styles can be offered in any order the editor prefers.
	core::Door::OpenStyle const openStyleChoices[] =
	{
		core::Door::OpenStyle::OpenUp,
		core::Door::OpenStyle::OpenLeft,
		core::Door::OpenStyle::OpenRight,
		core::Door::OpenStyle::OpenApart
	};
	char const* const openStyleItems[] =
	{
		doorOpenStyleLabel(openStyleChoices[0]),
		doorOpenStyleLabel(openStyleChoices[1]),
		doorOpenStyleLabel(openStyleChoices[2]),
		doorOpenStyleLabel(openStyleChoices[3])
	};
	int openStyleIndex = 0;
	for (size_t i = 0; i < sizeof(openStyleChoices) / sizeof(openStyleChoices[0]); ++i)
		if (openStyleChoices[i] == door->getOpenStyle()) openStyleIndex = static_cast<int>(i);
	if (ImGui::Combo("Opening", &openStyleIndex, openStyleItems,
		static_cast<int>(sizeof(openStyleItems) / sizeof(openStyleItems[0]))))
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			gUISettings.worldPaused = true;
			std::string diagnostic;
			// A Lift- or Shuttle-owned Door has no Door record of its own; the
			// edit lands as an override in the transport's own record, leaving
			// sibling Doors alone.
			bool changed = liftOwned
				? building->setLiftStopDoorOpenStyle(liftSector, stopIndex,
						openStyleChoices[openStyleIndex], &diagnostic)
				: shuttleOwned
					? building->setShuttleDoorOpenStyle(liftSector, stopIndex, carriageIndex,
							doorIndex, openStyleChoices[openStyleIndex], &diagnostic)
					: building->setSectorDoorOpenStyle(door->getFrontSector()->getLayerIndex(),
							object->getCellY(), object->getCellX(), door->getCellsWide(),
							openStyleChoices[openStyleIndex], &diagnostic);
			if (!changed)
				throw runtime_error(diagnostic.empty()
					? "Could not change the Door's opening style" : diagnostic);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Door editor", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Door editor", 0, core::LogLevel::Error, error.what());
		}
	}
	if (!building->isSimulationPaused())
		ImGui::TextDisabled("Pause simulation to change the opening style.");
	ImGui::EndDisabled();

	ImGui::Separator();
	ImGui::BeginDisabled(!building->isSimulationPaused() || liftOwned || shuttleOwned);
	if (ImGui::Button("Add Door Button"))
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			gUISettings.worldPaused = true;
			auto owner = object->getSector();
			uint32_t objectIndex{ ~0u };
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			{
				if (owner->getObject(i) == object)
				{
					objectIndex = i;
					break;
				}
			}
			if (objectIndex == ~0u)
				throw runtime_error("The selected Door no longer exists");
			building->addSectorDoorButton(owner->getIndex(), objectIndex);
			building->finishBuild();
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Door editor", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Door editor", 0, core::LogLevel::Error, error.what());
		}
	}
	ImGui::EndDisabled();
}
