// Door panel disabled-scope balance checks, for ticket #99.
//
// Selecting a Door leaked an ImGui disabled scope: the opening-style
// selector's BeginDisabled(!isSimulationPaused()) had no matching EndDisabled,
// so the Selection window ended one disabled-stack entry deep. Debug builds
// tripped ImGui's BeginDisabled/EndDisabled mismatch assertion; Release builds
// carried the global disabled flag and reduced alpha past the Door panel,
// dimming unrelated editor controls and leaking another entry every frame.
//
// The panel itself is compiled into this binary (src/DoorPanel.cpp), so these
// checks drive the real renderDoorPanel inside a CPU-side ImGui context -
// there is no mirrored copy to drift out of sync with the panel. Every Door
// ownership type (ordinary, Lift-owned, Shuttle-owned) is rendered while the
// simulation is paused and while it is running, and each render must leave:
//
//   * ImGui's disabled stack at the depth it started (checked explicitly, and
//     again by ImGui's own Debug end-window assertion when ImGui::End runs)
//   * the current item flags and global alpha untouched
//   * a control rendered after the panel free of inherited disabled state

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "DoorPanel.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "core/Building.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Sector.h"
#include "core/SectorObject.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct ImGuiGuard
	{
		ImGuiGuard()
		{
			ImGui::CreateContext();
			auto& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			// NewFrame() installs GetDefaultFont(), which reads Fonts[0]; a fresh
			// atlas has no fonts until one is added and built.
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	std::shared_ptr<const core::SectorObject> doorObjectAt(
		core::Building::CreateObjectResult const& created)
	{
		require(created.index != ~0u && created.sector != nullptr,
			"The test Building did not create its Door");
		auto const object = created.sector->getObject(created.index);
		require(object != nullptr && object->getObjectType() == core::SectorObjectType::Door,
			"The created object is not a Door");
		return object;
	}

	void requireOrdinary(core::Building const& building,
		std::shared_ptr<const core::SectorObject> const& object)
	{
		uint32_t liftSector{ ~0u }, stopIndex{ ~0u }, carriageIndex{ ~0u }, doorIndex{ ~0u };
		require(!building.isLiftOwnedDoor(object, &liftSector, &stopIndex),
			"The ordinary test Door reads as Lift-owned");
		require(!building.isShuttleOwnedDoor(object, &liftSector, &stopIndex,
				&carriageIndex, &doorIndex),
			"The ordinary test Door reads as Shuttle-owned");
	}

	void requireLiftOwned(core::Building const& building,
		std::shared_ptr<const core::SectorObject> const& object, uint32_t liftSector)
	{
		uint32_t ownerSector{ ~0u }, stopIndex{ ~0u }, carriageIndex{ ~0u }, doorIndex{ ~0u };
		require(building.isLiftOwnedDoor(object, &ownerSector, &stopIndex),
			"The Lift test Door does not read as Lift-owned");
		require(ownerSector == liftSector, "The Lift test Door names the wrong Lift sector");
		require(!building.isShuttleOwnedDoor(object, &ownerSector, &stopIndex,
				&carriageIndex, &doorIndex),
			"The Lift test Door also reads as Shuttle-owned");
	}

	void requireShuttleOwned(core::Building const& building,
		std::shared_ptr<const core::SectorObject> const& object, uint32_t shuttleSector)
	{
		uint32_t ownerSector{ ~0u }, stopIndex{ ~0u }, carriageIndex{ ~0u }, doorIndex{ ~0u };
		require(building.isShuttleOwnedDoor(object, &ownerSector, &stopIndex,
				&carriageIndex, &doorIndex),
			"The Shuttle test Door does not read as Shuttle-owned");
		require(ownerSector == shuttleSector,
			"The Shuttle test Door names the wrong Shuttle sector");
		require(!building.isLiftOwnedDoor(object, &ownerSector, &stopIndex),
			"The Shuttle test Door also reads as Lift-owned");
	}

	// Renders the real Door panel for one selected Door and requires every
	// side effect of a disabled scope - stack depth, item flags, global alpha -
	// to be exactly as the panel found them. In Debug builds ImGui::End()
	// additionally asserts through its own end-window stack check, so an
	// imbalance fails here twice over.
	void requirePanelLeavesNoDisabledState(std::shared_ptr<core::Building> const& building,
		std::shared_ptr<const core::SectorObject> object, char const* what)
	{
		ImGui::NewFrame();
		ImGui::Begin("Selection");

		auto const depthOnEntry = GImGui->DisabledStackSize;
		auto const flagsOnEntry = GImGui->CurrentItemFlags;
		auto const alphaOnEntry = GImGui->Style.Alpha;

		renderDoorPanel(building, object);

		require(GImGui->DisabledStackSize == depthOnEntry,
			std::string(what) + ": the panel left ImGui's disabled stack unbalanced");
		require(GImGui->CurrentItemFlags == flagsOnEntry,
			std::string(what) + ": the panel leaked item flags into the window");
		require(GImGui->Style.Alpha == alphaOnEntry,
			std::string(what) + ": the panel leaked a reduced global alpha");

		// A control rendered after the panel must inherit none of its disabled
		// state; this is exactly what the running-simulation leak dimmed.
		ImGui::Button("After the panel");
		require(!(GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled),
			std::string(what)
				+ ": a control rendered after the panel inherited a disabled scope");
		require(GImGui->Style.Alpha == alphaOnEntry,
			std::string(what) + ": a control after the panel rendered dimmed");

		ImGui::End();
		ImGui::Render();
	}

	void checkOrdinaryDoor()
	{
		auto building = std::make_shared<core::Building>("Ordinary door panel", 12, 3);
		building->addRoom("Fore", 0, 0, 0, 11, 2);
		building->addRoom("Aft", 1, 0, 0, 11, 2);
		auto const created = building->addSectorDoor(0, 0, 3, core::Building::CreateDoorOptions{});
		building->finishBuild();
		auto const object = doorObjectAt(created.door);
		requireOrdinary(*building, object);

		requirePanelLeavesNoDisabledState(building, object,
			"An ordinary Door selection, simulation running");
		building->pauseSimulation();
		requirePanelLeavesNoDisabledState(building, object,
			"An ordinary Door selection, simulation paused");
	}

	void checkExistingButtonsCanBeRemoved()
	{
		auto building = std::make_shared<core::Building>("Door button checkbox", 12, 3);
		building->addRoom("Fore", 0, 0, 0, 11, 2);
		building->addRoom("Aft", 1, 0, 0, 11, 2);
		core::Building::CreateDoorOptions options;
		options.controls[0] = true;
		options.controls[1] = true;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		auto const created = building->addSectorDoor(0, 0, 7, options);
		building->finishBuild();
		building->pauseSimulation();
		auto const object = doorObjectAt(created.door);

		ImGui::NewFrame();
		ImGui::Begin("Selection");
		renderDoorPanel(building, object);
		require(GImGui->LastItemData.ID == ImGui::GetID("Buttons"),
			"The Door panel's final control is not the Buttons checkbox");
		require(!(GImGui->LastItemData.InFlags & ImGuiItemFlags_Disabled),
			"A paused ordinary Door with Buttons cannot have them unchecked");
		ImGui::End();
		ImGui::Render();
	}

	void checkLiftOwnedDoor()
	{
		auto building = std::make_shared<core::Building>("Lift door panel", 16, 3);
		auto const hall = building->addRoom("Lift Hall", 0, 0, 0, 16, 3);
		for (uint32_t deck = 1; deck < 3; ++deck)
			for (uint32_t x = 0; x < 16; ++x)
				building->addSectorWalkway(hall, deck, x);
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		options.decksHigh = 3;
		options.stopOffsets = { 0, 1, 2 };
		auto const lift = building->addLift(1, 0, 8, options);
		require(lift.doors.size() == 3, "The Lift did not generate one Door per stop");
		building->finishBuild();
		auto const object = doorObjectAt(lift.doors[0].door);
		requireLiftOwned(*building, object, lift.lift.sector->getIndex());

		requirePanelLeavesNoDisabledState(building, object,
			"A Lift-owned Door selection, simulation running");
		building->pauseSimulation();
		requirePanelLeavesNoDisabledState(building, object,
			"A Lift-owned Door selection, simulation paused");
	}

	void checkShuttleOwnedDoor()
	{
		auto building = std::make_shared<core::Building>("Shuttle door panel", 32, 3);
		building->addCorridor(0, 0, 31);
		building->addCorridor(1, 0, 31);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101;
		auto const shuttle = building->addShuttle(1, 0, 0, 27, options);
		building->finishBuild();
		// The grid is stop x carriage x doorMask cell, with empty cells where a
		// partial landing is unsupported; take the first Door it actually made.
		auto const made = std::find_if(shuttle.doors.begin(), shuttle.doors.end(),
			[](core::Building::CreateDoorResult const& entry)
			{ return entry.door.index != ~0u && entry.door.sector != nullptr; });
		require(made != shuttle.doors.end(), "The Shuttle generated no Doors at all");
		auto const object = doorObjectAt(made->door);
		requireShuttleOwned(*building, object, shuttle.shuttle.sector->getIndex());

		requirePanelLeavesNoDisabledState(building, object,
			"A Shuttle-owned Door selection, simulation running");
		building->pauseSimulation();
		requirePanelLeavesNoDisabledState(building, object,
			"A Shuttle-owned Door selection, simulation paused");
	}
}

void runDoorPanelScopeSmokeChecks()
{
	ImGuiGuard guard;
	checkOrdinaryDoor();
	checkExistingButtonsCanBeRemoved();
	checkLiftOwnedDoor();
	checkShuttleOwnedDoor();
}
