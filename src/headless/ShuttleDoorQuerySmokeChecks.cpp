// Independent-optionality checks for World::isShuttleOwnedDoor, ticket #104.
//
// The query's outputs - Shuttle sector, stop, carriage, and compacted Door
// index - are each independently optional. Resolving the compacted Door index
// walks the Shuttle's authored construction record and used to reuse the
// caller's shuttleSectorIndex output pointer as its working state, so asking
// for doorIndex alone dereferenced a null pointer and killed the process.
//
// These checks drive the public query over every subset of outputs a caller
// may request on a real generated Shuttle Door and require each answered field
// to equal the value the full query returns. The compacted index itself is
// checked against the physical cell the Door was generated on: it is the
// position of the Door's cell within the carriage's selected doorMask cells,
// which is the grid setShuttleDoorOpenStyle addresses. Non-Shuttle Doors are
// queried the same way and must simply answer "not mine".

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/World.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Sector.h"
#include "core/SectorObject.h"
#include "core/SimulationCoordinator.h"

namespace
{
	constexpr uint32_t kUnset = 0xA5A5A5u;

	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct Ownership
	{
		bool owned{ false };
		uint32_t shuttleSector{ kUnset };
		uint32_t stopIndex{ kUnset };
		uint32_t carriageIndex{ kUnset };
		uint32_t doorIndex{ kUnset };
	};

	// Bit positions of the optional outputs, so a mask states exactly which
	// output pointers the caller supplies and every other one stays null.
	enum Output : uint32_t
	{
		Sector = 1u << 0,
		Stop = 1u << 1,
		Carriage = 1u << 2,
		Door = 1u << 3,
		All = Sector | Stop | Carriage | Door,
	};

	Ownership query(core::World const& world,
		std::shared_ptr<const core::SectorObject> const& object, uint32_t mask)
	{
		uint32_t sector{ kUnset }, stop{ kUnset }, carriage{ kUnset }, door{ kUnset };
		Ownership result{};
		result.owned = world.isShuttleOwnedDoor(object,
			(mask & Output::Sector) ? &sector : nullptr,
			(mask & Output::Stop) ? &stop : nullptr,
			(mask & Output::Carriage) ? &carriage : nullptr,
			(mask & Output::Door) ? &door : nullptr);
		result.shuttleSector = sector;
		result.stopIndex = stop;
		result.carriageIndex = carriage;
		result.doorIndex = door;
		return result;
	}

	char const* fieldName(uint32_t bit)
	{
		switch (bit)
		{
		case Output::Sector: return "shuttleSectorIndex";
		case Output::Stop: return "stopIndex";
		case Output::Carriage: return "carriageIndex";
		default: return "doorIndex";
		}
	}

	uint32_t fieldOf(Ownership const& value, uint32_t bit)
	{
		switch (bit)
		{
		case Output::Sector: return value.shuttleSector;
		case Output::Stop: return value.stopIndex;
		case Output::Carriage: return value.carriageIndex;
		default: return value.doorIndex;
		}
	}

	// Every subset of the optional outputs must answer the fields it asked for
	// with the reference values; the full query is the reference.
	void requireOutputSubsets(core::World const& world,
		std::shared_ptr<const core::SectorObject> const& object,
		Ownership const& want, std::string const& what)
	{
		for (uint32_t mask = 0; mask <= Output::All; ++mask)
		{
			auto const got = query(world, object, mask);
			require(got.owned == want.owned,
				what + ": ownership answer changed with output mask " + std::to_string(mask));
			if (!got.owned) continue;
			for (auto const bit : { Output::Sector, Output::Stop, Output::Carriage, Output::Door })
			{
				if ((mask & bit) == 0) continue;
				require(fieldOf(got, bit) == fieldOf(want, bit),
					what + ": output mask " + std::to_string(mask) + " answered "
					+ fieldName(bit) + " as " + std::to_string(fieldOf(got, bit))
					+ ", expected " + std::to_string(fieldOf(want, bit)));
			}
		}
	}

	std::shared_ptr<const core::SectorObject> requireDoor(
		core::World::CreateObjectResult const& created, char const* what)
	{
		require(created.index != ~0u && created.sector != nullptr,
			std::string(what) + " did not create a Door");
		auto const object = created.sector->getObject(created.index);
		require(object != nullptr && object->getObjectType() == core::SectorObjectType::Door,
			std::string(what) + " is not a Door");
		return object;
	}

	// Every Shuttle-owned Door in the World, keyed by its grid identity.
	std::map<std::array<uint32_t, 3>, std::shared_ptr<const core::SectorObject>>
		shuttleDoorGrid(core::World const& world, uint32_t shuttleSector)
	{
		std::map<std::array<uint32_t, 3>, std::shared_ptr<const core::SectorObject>> grid;
		for (uint32_t sectorIndex = 0; sectorIndex < world.getNumSectors(); ++sectorIndex)
		{
			auto const sector = world.getSector(sectorIndex);
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
				auto const owned = query(world, object, Output::All);
				if (!owned.owned || owned.shuttleSector != shuttleSector) continue;
				grid[{ owned.stopIndex, owned.carriageIndex, owned.doorIndex }] = object;
			}
		}
		return grid;
	}

	// A two-stop, two-carriage Shuttle with two selected door cells per carriage:
	// the compacted door index must span 0..1 per carriage and never depend on
	// another output being requested.
	void checkShuttleDoorOutputsAreIndependent()
	{
		uint32_t const numCars = 2;
		uint32_t const carWidth = 3;
		uint32_t const doorMask = 0b101;
		std::vector<uint32_t> const stopOffsets{ 0, 18 };
		uint32_t const shuttleX = 0;

		auto world = std::make_shared<core::World>("Shuttle door query", 32, 3);
		world->addCorridor(0, 0, 31);
		world->addCorridor(1, 0, 31);
		core::World::CreateShuttleOptions options{ numCars, carWidth, stopOffsets, 0 };
		options.capacity = 2;
		options.doorMask = doorMask;
		auto const shuttle = world->addShuttle(1, 0, shuttleX, 27, options);
		world->finishBuild();

		auto const shuttleSector = shuttle.shuttle.sector->getIndex();
		auto const offsets = core::SimulationCoordinator::shuttleDoorOffsets(carWidth, doorMask);
		require(offsets.size() == 2, "The test Shuttle doorMask does not select two cells");
		uint32_t const doorCount = static_cast<uint32_t>(offsets.size());
		uint32_t const expectedDoors = static_cast<uint32_t>(stopOffsets.size()) * numCars * doorCount;

		auto const grid = shuttleDoorGrid(*world, shuttleSector);
		require(grid.size() == expectedDoors,
			"The Shuttle generated " + std::to_string(grid.size())
			+ " owned Doors, expected " + std::to_string(expectedDoors));

		// The compacted index is dense per carriage and names the physical cell
		// the Door was generated on.
		for (uint32_t stop = 0; stop < stopOffsets.size(); ++stop)
			for (uint32_t car = 0; car < numCars; ++car)
			{
				std::set<uint32_t> indices;
				for (uint32_t door = 0; door < doorCount; ++door)
				{
					auto const found = grid.find({ stop, car, door });
					require(found != grid.end(),
						"Stop " + std::to_string(stop) + ", carriage " + std::to_string(car)
						+ " has no Door at compacted index " + std::to_string(door));
					indices.insert(door);
					auto const expectedX = shuttleX + stopOffsets[stop]
						+ car * (carWidth + 1) + offsets[door];
					require(found->second->getCellX() == expectedX,
						"Compacted door index " + std::to_string(door) + " names cell "
						+ std::to_string(found->second->getCellX()) + ", expected "
						+ std::to_string(expectedX));
				}
				require(indices.size() == doorCount,
					"Stop " + std::to_string(stop) + ", carriage " + std::to_string(car)
					+ " does not cover every selected door cell");
			}

		// The core regression: doorIndex alone, with shuttleSectorIndex null,
		// used to dereference that null pointer.
		for (auto const& [key, object] : grid)
		{
			auto const want = query(*world, object, Output::All);
			require(want.owned, "The Shuttle Door stopped reading as Shuttle-owned");
			require(want.shuttleSector == shuttleSector,
				"The Shuttle Door names the wrong Shuttle sector");
			require(want.doorIndex != ~0u && want.doorIndex < doorCount,
				"The Shuttle Door has no compacted door index");

			auto const doorOnly = query(*world, object, Output::Door);
			require(doorOnly.owned,
				"Requesting only doorIndex stopped reporting the Door as Shuttle-owned");
			require(doorOnly.doorIndex == want.doorIndex,
				"Requesting only doorIndex returned " + std::to_string(doorOnly.doorIndex)
				+ ", expected " + std::to_string(want.doorIndex));

			// Every other subset - single fields, pairs, triples, and the full
			// query - must answer identically.
			requireOutputSubsets(*world, object, want,
				"Stop " + std::to_string(key[0]) + ", carriage " + std::to_string(key[1])
				+ ", door " + std::to_string(key[2]));
		}
	}

	// Doors the Shuttle does not own answer "not mine" for every output subset,
	// including doorIndex alone.
	void requireNotShuttleOwned(core::World const& world,
		std::shared_ptr<const core::SectorObject> const& object, char const* what)
	{
		for (uint32_t mask = 0; mask <= Output::All; ++mask)
		{
			auto const got = query(world, object, mask);
			require(!got.owned, std::string(what)
				+ ": reads as Shuttle-owned with output mask " + std::to_string(mask));
		}
	}

	void checkLiftOwnedDoorIsNotShuttleOwned()
	{
		auto world = std::make_shared<core::World>("Lift door query", 16, 3);
		auto const hall = world->addRoom("Lift Hall", 0, 0, 0, 16, 3);
		for (uint32_t level = 1; level < 3; ++level)
			for (uint32_t x = 0; x < 16; ++x)
				world->addSectorWalkway(hall, level, x);
		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.levelsHigh = 3;
		liftOptions.stopOffsets = { 0, 1, 2 };
		auto const lift = world->addLift(1, 0, 8, liftOptions);
		require(lift.doors.size() == 3, "The Lift did not generate one Door per stop");
		world->finishBuild();
		auto const object = requireDoor(lift.doors[0].door, "The Lift Door");
		require(world->isLiftOwnedDoor(object),
			"The test Lift Door does not read as Lift-owned");
		requireNotShuttleOwned(*world, object, "A Lift-owned Door");
	}

	void checkOrdinaryDoorIsNotShuttleOwned()
	{
		auto world = std::make_shared<core::World>("Ordinary door query", 12, 3);
		world->addRoom("Fore", 0, 0, 0, 11, 2);
		world->addRoom("Aft", 1, 0, 0, 11, 2);
		auto const ordinary = world->addSectorDoor(0, 0, 3, core::World::CreateDoorOptions{});
		world->finishBuild();
		auto const object = requireDoor(ordinary.door, "The ordinary Door");
		require(!world->isLiftOwnedDoor(object), "The ordinary Door reads as Lift-owned");
		requireNotShuttleOwned(*world, object, "An ordinary Door");
	}
}

void runShuttleDoorQuerySmokeChecks()
{
	checkShuttleDoorOutputsAreIndependent();
	checkLiftOwnedDoorIsNotShuttleOwned();
	checkOrdinaryDoorIsNotShuttleOwned();
}
