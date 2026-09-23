#include "RecentFiles.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

using namespace std;

RecentFiles::RecentFiles(size_t maximumEntries)
	: mMaximumEntries(maximumEntries)
{
	if (maximumEntries == 0) throw invalid_argument("RecentFiles requires a positive entry limit");
}

void RecentFiles::initialize(filesystem::path filepath)
{
	mFilepath = std::move(filepath);
	mEntries.clear();

	ifstream input(mFilepath);
	if (!input)
	{
		if (filesystem::exists(mFilepath))
			throw runtime_error("Could not read recent-file list: " + mFilepath.string());
		save();
		return;
	}

	for (string line; getline(input, line);)
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || find(mEntries.begin(), mEntries.end(), line) != mEntries.end()) continue;
		mEntries.push_back(std::move(line));
		if (mEntries.size() == mMaximumEntries) break;
	}
	if (input.bad()) throw runtime_error("Could not read recent-file list: " + mFilepath.string());
}

void RecentFiles::add(string filepath)
{
	if (mFilepath.empty()) throw logic_error("RecentFiles must be initialized before use");
	if (filepath.empty()) return;
	mEntries.erase(std::remove(mEntries.begin(), mEntries.end(), filepath), mEntries.end());
	mEntries.push_front(std::move(filepath));
	while (mEntries.size() > mMaximumEntries) mEntries.pop_back();
	save();
}

bool RecentFiles::removeUnavailable(string const& filepath)
{
	if (mFilepath.empty()) throw logic_error("RecentFiles must be initialized before use");
	error_code error;
	auto const status = filesystem::status(filepath, error);
	if ((!error && filesystem::is_regular_file(status))
		|| (error && error != errc::no_such_file_or_directory)) return false;

	auto const previousSize = mEntries.size();
	mEntries.erase(std::remove(mEntries.begin(), mEntries.end(), filepath), mEntries.end());
	if (mEntries.size() == previousSize) return false;
	save();
	return true;
}

void RecentFiles::save() const
{
	ofstream output(mFilepath, ios::trunc);
	if (!output) throw runtime_error("Could not write recent-file list: " + mFilepath.string());
	for (auto const& entry : mEntries) output << entry << '\n';
	if (!output) throw runtime_error("Could not write recent-file list: " + mFilepath.string());
}
