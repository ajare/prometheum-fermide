#pragma once

#include <string>
#include <memory>

#include "core/Area.h"
#include "core/Object.h"
#include "core/SectorObjectType.h"


namespace core
{

	class Vertex;
	class Sector;
	class World;

	// Intended as a base class for Interactables, Windows, etc.
	class SectorObject : public Area
	{
		static uint32_t VertexIdentifierGenerator;

		SectorObjectType mObjectType;

		std::shared_ptr<const Sector> mSector;

		std::shared_ptr<Object> mObject;

		uint32_t mVertexIdentifier;

	public:

		SectorObject(SectorObjectType type, std::shared_ptr<const Sector> sector, uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh, std::shared_ptr<Object> object, uint32_t* vertexIdentifer);

		~SectorObject() = default;

		SectorObjectType getObjectType() const;

		std::shared_ptr<const Sector> getSector() const;

		std::shared_ptr<Object> _getObject() const;

		std::string getDescription() const;

		uint32_t getVertexIdentifier() const;

		bool pointInside(float x, float y) const;

		virtual std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const = 0;
		
		virtual void update(float frameTime);
	};

} // core
