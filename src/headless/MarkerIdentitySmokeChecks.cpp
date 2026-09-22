#include <memory>
#include <stdexcept>
#include <string>

#include "core/Building.h"
#include "core/MarkerSectorObject.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeBuilding(core::Building const& building)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		building.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::unique_ptr<core::Building> deserializeBuilding(std::string const& yaml)
	{
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		auto building = std::make_unique<core::Building>("placeholder", 1, 1);
		core::SerializationWorkData work;
		require(building->deserialize(*reader, work), "A complete Building did not deserialize");
		return building;
	}

	void markersHaveValidatedStableIdentity()
	{
		core::Building building("Marker identity", 12, 2);
		auto room = building.addRoom("Room", 0, 0, 0, 12, 1);
		building.addSectorMarker(room, 0, 1.5f, "  Entrance  ");
		building.addSectorMarker(room, 0, 3.5f, "entrance");
		auto ids = building.getMarkerIds();
		require(ids.size() == 2 && ids[0].value != 0 && ids[1].value > ids[0].value,
			"New Markers did not receive monotonic nonzero IDs");
		require(building.lookupMarker(ids[0])->getName() == "Entrance",
			"Marker creation did not store the trimmed name");
		require(building.lookupMarker(ids[1])->getName() == "entrance",
			"Marker name uniqueness was not case-sensitive");

		std::string diagnostic;
		require(!building.canRenameMarker(ids[1], " Entrance ", &diagnostic),
			"A duplicate Marker name was accepted");
		require(!building.canRenameMarker(ids[1], "\xC0\x80", &diagnostic),
			"Malformed UTF-8 was accepted as a Marker name");
		require(building.renameMarker(ids[0], "  Lobby  ", &diagnostic),
			"A valid Marker rename was refused: " + diagnostic);
		require(building.lookupMarker(ids[0])->getName() == "Lobby",
			"Marker rename changed or lost the destination");

		auto const beforeDelete = serializeBuilding(building);
		auto firstObject = building.getSector(room)->getObject(0);
		require(firstObject && building.removeSectorMarker(room, 0),
			"Marker deletion through Building failed");
		require(!building.lookupMarker(ids[0]), "A deleted Marker identity still resolves");
		auto restored = deserializeBuilding(beforeDelete);
		require(restored->lookupMarker(ids[0])
			&& restored->lookupMarker(ids[0])->getName() == "Lobby",
			"Snapshot restoration did not restore the same Marker identity");

		building.addSectorMarker(room, 0, 5.5f, "Replacement");
		auto afterDelete = building.getMarkerIds();
		require(afterDelete.back().value > ids.back().value,
			"A deleted Marker ID was reused");

		core::Building highestDeleted("Deleted high-water mark", 4, 1);
		auto corridor = highestDeleted.addCorridor(0, 0, 4);
		auto only = highestDeleted.addSectorMarker(corridor, 0, 1.5f, "Only");
		auto deletedId = highestDeleted.getMarkerIds().front();
		require(highestDeleted.removeSectorMarker(corridor, only.index),
			"The highest Marker could not be deleted");
		auto reopened = deserializeBuilding(serializeBuilding(highestDeleted));
		reopened->pauseSimulation();
		reopened->addSectorMarker(corridor, 0, 2.5f, "Later");
		require(reopened->getMarkerIds().front().value > deletedId.value,
			"Save/load reused the highest deleted Marker ID");
	}

	void identitySurvivesReplayAndCompleteSerialization()
	{
		core::Building building("Marker replay", 12, 1);
		auto left = building.addCorridor(0, 0, 5);
		auto right = building.addCorridor(0, 7, 5);
		auto created = building.addSectorMarker(left, 0, 2.5f, "Destination");
		auto id = building.getMarkerIds().front();
		building.finishBuild();
		building.pauseSimulation();
		auto plan = building.planMoveSectorObject(left, created.index, 9, 0);
		require(plan.valid, "A Marker move could not be planned: " + plan.diagnostic);
		require(static_cast<bool>(building.applyObjectMove(plan)), "A Marker move could not be applied");
		require(building.lookupMarker(id) && building.lookupMarker(id)->getName() == "Destination",
			"Object replay changed Marker identity");

		auto reopened = deserializeBuilding(serializeBuilding(building));
		require(reopened->lookupMarker(id) && reopened->lookupMarker(id)->getName() == "Destination",
			"Save/load did not preserve Marker identity and name");
		auto movedIds = reopened->getMarkerIds();
		require(movedIds.size() == 1 && movedIds.front() == id,
			"Save/load changed the Marker ID");

		auto moved = reopened->lookupMarker(id);
		require(moved && moved->getCellX() == 9, "The moved Marker did not round-trip");
		(void)right;
	}
}

void runMarkerIdentitySmokeChecks()
{
	markersHaveValidatedStableIdentity();
	identitySurvivesReplayAndCompleteSerialization();
}
