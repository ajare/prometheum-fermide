#pragma once

#include <filesystem>
#include <string_view>

namespace core
{
	inline constexpr std::string_view WorldDocumentFilenameSuffix{ ".world.yaml" };

	bool isWorldDocumentPath(std::filesystem::path const& filepath);
	void requireWorldDocumentPath(std::filesystem::path const& filepath);

	// Returns the path with the complete .world.yaml suffix removed. For example,
	// /project/station.world.yaml becomes /project/station.
	std::filesystem::path worldDocumentBasePath(
		std::filesystem::path const& filepath);
}
