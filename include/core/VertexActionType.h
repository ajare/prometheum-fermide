#pragma once

#include <string>


namespace core
{

	enum struct VertexActionType
	{
		None,
		Wait,
		TraverseEdge,
		UnblockForExit,
		UseController,
		LookOutOfWindow
	};

	std::string getVertexActionTypeString(VertexActionType type);

} // core
