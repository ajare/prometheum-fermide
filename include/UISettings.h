#pragma once


struct UISettings
{
	enum Style
	{
		Light,
		Dark,
		Classic
	};

	enum SelectionMode
	{
		Object,
		Vertex
	};

	Style style{ Style::Dark };

	SelectionMode selectionMode{ SelectionMode::Object };

	int visibleLayer{ 0 };

	bool renderGrid{ false };

	bool renderGraph{ false };

	bool renderNonVisibleLayer{ false };

	bool highlightNearestVertex{ false };

	bool renderAgentDebug{ true };

	float xOffset{ 32 };

	float yOffset{ 32 };

	// Screen-space rectangle occupied by the docked world view.
	float worldViewportX{ 0 };
	float worldViewportY{ 0 };
	float worldViewportWidth{ 0 };
	float worldViewportHeight{ 0 };

	bool worldPaused{ false };
};
