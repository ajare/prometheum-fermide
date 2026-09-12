#pragma once

#include <map>
#include <string>
#include <memory>

#include "core/Area.h"
#include "core/Object.h"
#include "core/SectorObjectType.h"


namespace core
{

	class Vertex;
	class VertexController;
	class Sector;
	class Building;

	// Intended as a base class for Interactables, Windows, etc.
	class SectorObject : public Area
	{
		static uint32_t VertexIdentifierGenerator;

		SectorObjectType mObjectType;

		std::shared_ptr<const Sector> mSector;

		std::shared_ptr<Object> mObject;

		uint32_t mVertexIdentifier;

		std::map<std::string, std::shared_ptr<Controller>> mControllerLookup;

	public:

		SectorObject(SectorObjectType type, std::shared_ptr<const Sector> sector, uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh, std::shared_ptr<Object> object, uint32_t* vertexIdentifer);

		~SectorObject() = default;

		SectorObjectType getObjectType() const;

		std::shared_ptr<const Sector> getSector() const;

		std::shared_ptr<Object> _getObject() const;

		std::string getDescription() const;

		uint32_t getVertexIdentifier() const;

		bool hasController(std::string const& key) const;

		std::shared_ptr<Controller> getController(std::string const& key) const;

		void addController(std::string const& key, std::shared_ptr<Controller> controller);

		bool pointInside(float x, float y) const;

		virtual std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const = 0;
		
		virtual std::shared_ptr<VertexController> createVertexController(Building const* building, std::vector<std::shared_ptr<Vertex>> const& vertices, std::map<std::shared_ptr<Controller>, std::shared_ptr<Vertex>> const& controllerVertexLookup) const;

		virtual void update(float frameTime);
	};

} // core
