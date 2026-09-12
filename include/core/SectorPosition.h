#pragma once

#include "core/Vector2.h"


namespace core
{
	class Sector;

	class SectorPosition
	{
		Sector const* mSector;

		Vector2 mPosition;

	public:

		SectorPosition();

		SectorPosition(Sector const* sector, float x, float y);

		SectorPosition(Sector const* sector, Vector2 const& pos);

		Sector const* sector() const;

		Vector2 const& local() const;

		Vector2 global() const;
	};

} // core
