#pragma once

#include <cstddef>
#include <deque>
#include <filesystem>
#include <string>

class RecentFiles
{
	std::filesystem::path mFilepath;
	std::deque<std::string> mEntries;
	std::size_t mMaximumEntries;

	void save() const;

public:
	explicit RecentFiles(std::size_t maximumEntries = 5);

	// Loads an existing list, or creates an empty file when none exists.
	void initialize(std::filesystem::path filepath);
	void add(std::string filepath);
	// Removes and persists a selected entry only when it no longer resolves to
	// a regular file. Inspection errors leave the entry intact.
	bool removeUnavailable(std::string const& filepath);

	[[nodiscard]] bool empty() const { return mEntries.empty(); }
	[[nodiscard]] std::deque<std::string> const& entries() const { return mEntries; }
	[[nodiscard]] std::filesystem::path const& filepath() const { return mFilepath; }
};
