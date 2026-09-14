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
	mEntries.erase(remove(mEntries.begin(), mEntries.end(), filepath), mEntries.end());
	mEntries.push_front(std::move(filepath));
	while (mEntries.size() > mMaximumEntries) mEntries.pop_back();
	save();
}

void RecentFiles::save() const
{
	ofstream output(mFilepath, ios::trunc);
	if (!output) throw runtime_error("Could not write recent-file list: " + mFilepath.string());
	for (auto const& entry : mEntries) output << entry << '\n';
	if (!output) throw runtime_error("Could not write recent-file list: " + mFilepath.string());
}
