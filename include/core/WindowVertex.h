#pragma once

#include "core/Vertex.h"
#include "core/Window.h"


namespace core
{
	class Location;

	class WindowVertex : public Vertex
	{
		std::shared_ptr<const Window> mWindow;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		WindowVertex(uint32_t id, std::shared_ptr<Sector> sector, std::shared_ptr<const Window> window, float xLocationOffset, float yLocationOffset);

		WindowVertex(std::shared_ptr<Sector> sector, std::shared_ptr<const Window> window, float xLocationOffset, float yLocationOffset);

		std::shared_ptr<const Window> getWindow() const;

		// Overridden from Vertex
		std::shared_ptr<Vertex> copyWithoutEdges() override;

		// Overridden from Vertex
		std::string getDescription() const override;
	};

} // core
