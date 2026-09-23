#include "core/WorldDocument.h"

#include "core/SerializationException.h"

namespace core
{
	bool isWorldDocumentPath(std::filesystem::path const& filepath)
	{
		auto const filename = filepath.filename().string();
		return filename.size() > WorldDocumentFilenameSuffix.size()
			&& filename.ends_with(WorldDocumentFilenameSuffix);
	}

	void requireWorldDocumentPath(std::filesystem::path const& filepath)
	{
		if (!isWorldDocumentPath(filepath))
		{
			throw SerializationException(
				"A World document file must end with .world.yaml");
		}
	}

	std::filesystem::path worldDocumentBasePath(
		std::filesystem::path const& filepath)
	{
		requireWorldDocumentPath(filepath);
		auto filename = filepath.filename().string();
		filename.erase(filename.size() - WorldDocumentFilenameSuffix.size());
		return filepath.parent_path() / filename;
	}
}
