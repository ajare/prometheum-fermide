// Two-sided Door Buttons, for ticket #125.
//
// Giving a Door a Button must give it Buttons on both sides of the threshold:
// agents approaching from the Layer behind were stuck, because the single
// Button sat on the front side only. These checks drive the real World
// edit actions and renderer:
//
//   * Add Door Button creates the missing Button on each side, binds both to
//     the Door's single traversal resource with idempotent open-only commands,
//     and records the pre-Button activation mode for removal
//   * the add is all-or-nothing: a side with no space for a Button refuses
//     the whole edit before anything is created
//   * a second add is refused once both sides carry a Button
//   * existing authored/YAML Buttons can be removed, falling back to manual
//     activation when no pre-Button mode was recorded; editor-added Buttons
//     restore the Door's recorded pre-Button activation mode
//   * serialization round-trips both Buttons and the recorded mode
//   * agents on both sides press their side's Button and cross, and every
//     device command either Button issues is an open
//   * the back-side Button renders as an outline only - including when its
//     own Layer is the one drawn solid - while the front-side Button fills

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "imgui/imgui.h"

#include "Render.h"
#include "UISettings.h"

#include "core/World.h"
#include "core/Button.h"
#include "core/Coordination.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct DoorLocation
	{
		uint32_t sectorIndex{ ~0u };
		uint32_t objectIndex{ ~0u };
		std::shared_ptr<const core::DoorSectorObject> object;
	};

	// The ordinary Door in a freshly built two-Room scene, wherever it
	// registered itself.
	DoorLocation findDoor(core::World const& world)
	{
		for (uint32_t s = 0; s < world.getNumSectors(); ++s)
		{
			auto sector = world.getSector(s);
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
				return { s, i, std::static_pointer_cast<const core::DoorSectorObject>(object) };
			}
		}
		return {};
	}

	std::vector<std::shared_ptr<const core::Button>> buttonsIn(core::World const& world,
		uint32_t sectorIndex)
	{
		std::vector<std::shared_ptr<const core::Button>> buttons;
		auto sector = world.getSector(sectorIndex);
		require(sector != nullptr, "The Sector vanished");
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto const object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::InteractionPoint)
				continue;
			buttons.push_back(std::static_pointer_cast<const core::Button>(object->_getObject()));
		}
		return buttons;
	}

	// Two Rooms on adjacent Layers with one ordinary Door between them.
	// doorOptions, when given, are applied to the Door at creation.
	struct Scene
	{
		std::unique_ptr<core::World> world;
		DoorLocation door;
		uint32_t frontSector{ ~0u };
		uint32_t backSector{ ~0u };

		core::World& operator*() const { return *world; }
		core::World* operator->() const { return world.get(); }
	};

	Scene buildTwoRoomScene(char const* name, uint32_t foreWidth = 8, uint32_t foreX = 0,
		core::World::CreateDoorOptions const* doorOptions = nullptr, uint32_t doorX = ~0u)
	{
		Scene scene;
		scene.world = std::make_unique<core::World>(name, 16, 2);
		while (scene.world->getLayerCount() < 2) scene.world->addLayer();
		scene.world->addRoom("Fore", 0, 0, foreX, foreWidth, 1);
		scene.world->addRoom("Aft", 1, 0, 0, 16, 1);
		if (doorX == ~0u) doorX = foreX + 2;
		auto const created = doorOptions
			? scene.world->addSectorDoor(0, 0, doorX, *doorOptions)
			: scene.world->addSectorDoor(0, 0, doorX, {});
		scene.world->finishBuild();
		scene.frontSector = created.door.sector->getIndex();
		scene.door = findDoor(*scene.world);
		require(scene.door.object != nullptr, "The scene's Door could not be found");
		scene.backSector = scene.door.object->getDoor()->getBackSector()->getIndex();
		return scene;
	}

	void requireBothSidesHaveOneButton(Scene const& scene, char const* what)
	{
		auto const front = buttonsIn(*scene.world, scene.frontSector);
		auto const back = buttonsIn(*scene.world, scene.backSector);
		require(front.size() == 1 && back.size() == 1,
			std::string(what) + ": the Door should carry exactly one Button per side");
		require(front[0]->getThresholdLayer() == 0 && back[0]->getThresholdLayer() == 0,
			std::string(what) + ": a Door Button did not inherit the Door's authored Layer");
	}

	void addingAButtonGivesBothSidesOne()
	{
		Scene scene = buildTwoRoomScene("Add door button");
		// Add from the back side's registration: the action must not care which
		// side of the pair the selected Sector sits on.
		uint32_t backObjectIndex{ ~0u };
		auto backSector = scene.world->getSector(scene.backSector);
		for (uint32_t i = 0; i < backSector->getNumObjects(); ++i)
			if (backSector->getObject(i) == scene.door.object) backObjectIndex = i;
		require(backObjectIndex != ~0u, "The Door is not registered on its back Sector");

		scene->pauseSimulation();
		require(scene->canAddSectorDoorButton(scene.backSector, backObjectIndex),
			"A Button-less Door should accept an added Door Button");
		scene->addSectorDoorButton(scene.backSector, backObjectIndex);
		scene->finishBuild();

		requireBothSidesHaveOneButton(scene, "Adding a Door Button");
		require(!scene->canAddSectorDoorButton(scene.backSector, backObjectIndex),
			"A both-sided Door should not accept another Door Button");
		require(scene->canRemoveSectorDoorButton(scene.backSector, backObjectIndex),
			"Editor-added Door Buttons should be removable");

		auto const resource = scene->lookupTraversalResource(
			scene.door.object->getDoor()->getTraversalResourceId());
		require(resource && resource.entity->getControls().size() == 2,
			"Both Buttons should bind to the Door's traversal resource");

		core::World::CreateDoorOptions options;
		require(scene->getSectorDoorOptions(0, 0, scene.door.object->getCellX(),
				scene.door.object->getDoor()->getCellsWide(), options),
			"The Door's record could not be read back");
		require(options.controls[0] && options.controls[1]
			&& options.activationMode == core::DoorActivationMode::RemoteControlled,
			"The Door record did not move to remote-controlled with both controls");
	}

	void addingCompletesALegacyOneSidedDoor()
	{
		// A Door authored with only its fore Button, as older builds produced.
		core::World::CreateDoorOptions authored;
		authored.controls[0] = true;
		authored.activationMode = core::DoorActivationMode::RemoteControlled;
		Scene scene = buildTwoRoomScene("Complete a legacy one-sided Door", 8, 0, &authored);
		require(buttonsIn(*scene.world, scene.frontSector).size() == 1
			&& buttonsIn(*scene.world, scene.backSector).empty(),
			"The authored Door should start with exactly its fore Button");

		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		requireBothSidesHaveOneButton(scene, "Completing a one-sided Door");
		require(scene->canRemoveSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"A completed legacy Door should allow its Buttons to be removed");
	}

	void addRefusesWhenASideHasNoSpace()
	{
		// The fore Room is exactly the Door's width, so no cell beside the Door
		// can take a Button on that side.
		Scene scene = buildTwoRoomScene("No space for a Button", 1, 5, nullptr, 5);
		require(scene.door.object->getCellX() == 5
			&& scene.door.object->getDoor()->getCellsWide() == 1,
			"The scene's Door should span its fore Room exactly");

		scene->pauseSimulation();
		auto const buttonsBefore = buttonsIn(*scene.world, scene.frontSector).size()
			+ buttonsIn(*scene.world, scene.backSector).size();
		bool refused = false;
		try
		{
			scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		}
		catch (core::Exception const&)
		{
			refused = true;
		}
		require(refused, "An add with no space on one side should be refused");
		auto const buttonsAfter = buttonsIn(*scene.world, scene.frontSector).size()
			+ buttonsIn(*scene.world, scene.backSector).size();
		require(buttonsBefore == 0 && buttonsAfter == 0,
			"A refused add must not leave a Button behind on either side");
		require(scene->canAddSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"A refused add should leave the Door addable");
	}

	void secondAddIsRefused()
	{
		Scene scene = buildTwoRoomScene("Second add refused");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		bool refused = false;
		try
		{
			scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		}
		catch (core::Exception const&)
		{
			refused = true;
		}
		require(refused, "A both-sided Door should refuse a second add");
		requireBothSidesHaveOneButton(scene, "A refused second add");
	}

	void authoredButtonsCanBeRemoved()
	{
		core::World::CreateDoorOptions authored;
		authored.controls[0] = true;
		authored.controls[1] = true;
		authored.activationMode = core::DoorActivationMode::RemoteControlled;
		Scene scene = buildTwoRoomScene("Authored Buttons", 8, 0, &authored);
		requireBothSidesHaveOneButton(scene, "The authored Door");

		scene->pauseSimulation();
		require(scene->canRemoveSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"Authored Buttons should read as removable");
		auto const rebuilt = scene->removeSectorDoorButton(scene.frontSector,
			scene.door.objectIndex);
		require(rebuilt != nullptr, "Removing authored Buttons should rebuild the Door");
		require(buttonsIn(*scene.world, scene.frontSector).empty()
			&& buttonsIn(*scene.world, scene.backSector).empty(),
			"Removing authored Buttons should take both away");

		core::World::CreateDoorOptions options;
		require(scene->getSectorDoorOptions(0, 0, rebuilt->getCellX(),
				rebuilt->getDoor()->getCellsWide(), options),
			"The Door record could not be read after removing authored Buttons");
		require(options.activationMode == core::DoorActivationMode::Manual,
			"A Door with removed authored Buttons should fall back to manual activation");
	}

	void removalRemovesBothButtonsAndRestoresTheMode()
	{
		Scene scene = buildTwoRoomScene("Remove door button");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();
		require(scene->canRemoveSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"Editor-added Buttons should read as removable");

		auto const doorX = scene.door.object->getCellX();
		auto const doorWidth = scene.door.object->getDoor()->getCellsWide();
		auto const rebuilt = scene->removeSectorDoorButton(scene.frontSector,
			scene.door.objectIndex);
		require(rebuilt != nullptr, "Removal should return the rebuilt Door");
		require(rebuilt->getCellX() == doorX && rebuilt->getDoor()->getCellsWide() == doorWidth,
			"Removal returned a Door at the wrong position");

		require(buttonsIn(*scene.world, scene.frontSector).empty()
			&& buttonsIn(*scene.world, scene.backSector).empty(),
			"Removal should take both Buttons away");
		auto const resource = scene->lookupTraversalResource(
			rebuilt->getDoor()->getTraversalResourceId());
		require(resource && resource.entity->getControls().empty(),
			"Removal should unbind every Button from the traversal resource");

		core::World::CreateDoorOptions options;
		require(scene->getSectorDoorOptions(0, 0, doorX, doorWidth, options),
			"The rebuilt Door's record could not be read back");
		require(!options.controls[0] && !options.controls[1]
			&& options.activationMode == core::DoorActivationMode::Manual,
			"Removal should restore the Door's pre-Button activation mode");

		uint32_t rebuiltIndex{ ~0u };
		auto sector = scene.world->getSector(scene.frontSector);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			if (sector->getObject(i) == rebuilt) rebuiltIndex = i;
		require(rebuiltIndex != ~0u, "The rebuilt Door is not on its front Sector");
		require(scene->canAddSectorDoorButton(scene.frontSector, rebuiltIndex),
			"The rebuilt Button-less Door should accept Buttons again");
		require(!scene->canRemoveSectorDoorButton(scene.frontSector, rebuiltIndex),
			"The rebuilt Door should not read as removable");
	}

	void serializationPreservesButtonsAndProvenance()
	{
		Scene scene = buildTwoRoomScene("Serialize two-sided Buttons");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		scene->serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("preButtonActivation") != std::string::npos,
			"The pre-Button activation mode did not persist");

		auto loaded = std::make_unique<core::World>("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		loaded->deserialize(*reader, workData);

		auto const loadedDoor = findDoor(*loaded);
		require(loadedDoor.object != nullptr, "The loaded World has no Door");
		require(buttonsIn(*loaded, loadedDoor.object->getDoor()->getFrontSector()->getIndex()).size() == 1
			&& buttonsIn(*loaded, loadedDoor.object->getDoor()->getBackSector()->getIndex()).size() == 1,
			"The loaded Door did not keep one Button per side");
		require(loaded->canRemoveSectorDoorButton(loadedDoor.sectorIndex, loadedDoor.objectIndex),
			"The loaded Door lost its Buttons' removal provenance");
	}

	void agentsOnBothSidesPressAndCross()
	{
		Scene scene = buildTwoRoomScene("Cross from both sides");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();
		require(scene->resumeSimulation(), "The scene should resume after its edit");
		auto const door = scene.door.object->getDoor();
		auto const doorResource = door->getTraversalResourceId();

		auto graph = scene->getGraph();
		auto foreTarget = graph->getClosestVertexInSector(
			scene.world->getSector(scene.frontSector).get(), { 2.0f, 0.0f });
		auto backTarget = graph->getClosestVertexInSector(
			scene.world->getSector(scene.backSector).get(), { 13.0f, 0.0f });
		require(foreTarget && backTarget, "The scene needs a target vertex on each side");

		auto const foreWalker = scene->createAgent("Fore walker", scene.frontSector, 0, 2.0f);
		auto const backWalker = scene->createAgent("Back walker", scene.backSector, 0, 13.0f);
		for (auto const& [id, target] : { std::pair{ foreWalker, backTarget },
			std::pair{ backWalker, foreTarget } })
		{
			auto agent = scene->lookupAgent(id).entity;
			require(agent != nullptr, "An Agent could not be created");
			auto path = graph->calculatePath(agent, target);
			require(path != nullptr, "An Agent's path could not be calculated");
			agent->setPath(path, true);
		}

		bool sawDoorOpen = false;
		bool sawOpenCommand = false;
		bool sawCloseCommand = false;
		for (uint32_t tick = 0; tick < 6000; ++tick)
		{
			scene->advanceTick();
			sawDoorOpen = sawDoorOpen || door->isOpen();
			auto const snapshot = scene->getSimulationSnapshot();
			for (auto const& operation : snapshot.deviceOperations)
			{
				if (operation.command.type != core::DeviceCommandType::OpenDoor
					|| operation.command.traversalResource != doorResource) continue;
				sawOpenCommand = sawOpenCommand || operation.command.desiredState;
				sawCloseCommand = sawCloseCommand || !operation.command.desiredState;
			}
			auto const foreIdle = scene->lookupAgent(foreWalker).entity->getState()
				== core::Agent::State::Idle;
			auto const backIdle = scene->lookupAgent(backWalker).entity->getState()
				== core::Agent::State::Idle;
			if (foreIdle && backIdle) break;
		}

		auto const foreAgent = scene->lookupAgent(foreWalker).entity;
		auto const backAgent = scene->lookupAgent(backWalker).entity;
		require(foreAgent->getState() == core::Agent::State::Idle
			&& backAgent->getState() == core::Agent::State::Idle,
			"Agents on both sides should finish their crossing");
		require(foreAgent->getGlobalPosition().distanceTo(backTarget->getPosition()) < 0.01f
			&& backAgent->getGlobalPosition().distanceTo(foreTarget->getPosition()) < 0.01f,
			"Agents on both sides should arrive at their destinations");
		require(sawDoorOpen, "The Door should have opened for the crossings");
		require(sawOpenCommand, "The Buttons should have issued open commands");
		require(!sawCloseCommand,
			"Neither Button may ever issue a close command: pressing the other one again must not close the Door");
	}

	ImU32 const kEnabledButtonColour = ImU32(ImColor(0, 255, 128));

	int countColourVertices(ImDrawList const* drawList, ImU32 colour)
	{
		int count = 0;
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
			if (drawList->VtxBuffer[i].col == colour) ++count;
		return count;
	}

	struct ImGuiGuard
	{
		ImGuiGuard() { ImGui::CreateContext(); }
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	ImDrawList* testDrawList()
	{
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 1280.0f;
		gUISettings.worldViewportHeight = 720.0f;
		return ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
	}

	void backButtonRendersAsOutlineOnly()
	{
		Scene scene = buildTwoRoomScene("Back Button rendering");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		auto const frontSector = scene.world->getSector(scene.frontSector);
		auto const backSector = scene.world->getSector(scene.backSector);
		ImColor const roomColour(192, 192, 255);

		// The front Layer drawn solid (the front Button's own Layer): the front
		// Button is a filled quad - exactly the four corners of one rect.
		{
			ImGuiGuard imgui;
			auto drawList = testDrawList();
			drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 720.0f), false);
			renderSector(frontSector, 0, LayerRenderStyle::Solid, false, roomColour, drawList);
			drawList->PopClipRect();
			require(countColourVertices(drawList, kEnabledButtonColour) == 4,
				"The front Button should render as one filled quad on the Door's authored Layer");
		}

		// The back Layer drawn solid (the back Button's own Layer): per the
		// Door rule, the back Button is still only an outline - a stroked rect
		// emits far more than the filled quad's four vertices.
		{
			ImGuiGuard imgui;
			auto drawList = testDrawList();
			drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 720.0f), false);
			renderSector(backSector, 1, LayerRenderStyle::Solid, false, roomColour, drawList);
			drawList->PopClipRect();
			auto const outlineVertices = countColourVertices(drawList, kEnabledButtonColour);
			require(outlineVertices > 4,
				"The back Button must never render solid, even on its own Layer");
		}

		// The wireframe overlay of the back Layer, as seen when the front Layer
		// is selected: the back Button shows through as the same outline.
		{
			ImGuiGuard imgui;
			auto drawList = testDrawList();
			drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 720.0f), false);
			renderSector(backSector, 1, LayerRenderStyle::Wireframe, false, roomColour, drawList);
			drawList->PopClipRect();
			require(countColourVertices(drawList, kEnabledButtonColour) > 4,
				"The back Button should render as an outline in the wireframe overlay");
		}
	}
}

void runDoorTwoSidedButtonSmokeChecks()
{
	addingAButtonGivesBothSidesOne();
	addingCompletesALegacyOneSidedDoor();
	addRefusesWhenASideHasNoSpace();
	secondAddIsRefused();
	authoredButtonsCanBeRemoved();
	removalRemovesBothButtonsAndRestoresTheMode();
	serializationPreservesButtonsAndProvenance();
	agentsOnBothSidesPressAndCross();
	backButtonRendersAsOutlineOnly();
}
