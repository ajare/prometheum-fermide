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

	enum VertexBoundsRenderMode
	{
		Never,
		Always,
		OnHover
	};

	Style style{ Style::Dark };

	SelectionMode selectionMode{ SelectionMode::Object };

	VertexBoundsRenderMode vertexBoundsRenderMode{ VertexBoundsRenderMode::Never };

	int visibleLayer{ 0 };

	bool renderGrid{ false };

	bool renderGraph{ false };

	bool renderNonVisibleLayer{ false };

	bool renderDoorQueueStops{ false };

	bool highlightNearestVertex{ false };

	bool renderAgentDebug{ true };

	float xOffset{ 32 };

	float yOffset{ 32 };

	bool worldPaused{ false };
};
