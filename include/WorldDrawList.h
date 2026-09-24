#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "imgui/imgui.h"

// CPU-side command stream for the World viewport. The GUI path submits these
// commands to MPP; the optional ImDrawList adapter exists only for the
// headless renderer checks and never participates in application rendering.
class WorldDrawList
{
public:
	enum class Texture
	{
		None,
		SectorAtlas,
		ObjectAtlas
	};

	struct ClipRectangle
	{
		ImVec2 minimum;
		ImVec2 maximum;
	};

	struct Line
	{
		ImVec2 from;
		ImVec2 to;
		ImU32 colour{};
		float thickness{ 1.0f };
		ClipRectangle clip;
	};

	struct Triangle
	{
		ImVec2 positions[3];
		ImVec2 texcoords[3];
		ImU32 colour{};
		Texture texture{ Texture::None };
		ClipRectangle clip;
	};

	struct Text
	{
		ImVec2 position;
		ImU32 colour{};
		std::string value;
		ClipRectangle clip;
	};

	struct Barrier {};
	using Command = std::variant<Line, Triangle, Text, Barrier>;

	explicit WorldDrawList(ClipRectangle initialClip);
	explicit WorldDrawList(ImDrawList* testAdapter);

	void PushClipRect(ImVec2 minimum, ImVec2 maximum, bool intersectWithCurrentClip = false);
	void PopClipRect();
	ImVec2 GetClipRectMin() const;
	ImVec2 GetClipRectMax() const;

	void AddLine(ImVec2 from, ImVec2 to, ImU32 colour, float thickness = 1.0f);
	void AddRect(ImVec2 minimum, ImVec2 maximum, ImU32 colour,
		float rounding = 0.0f, ImDrawFlags flags = 0, float thickness = 1.0f);
	void AddRectFilled(ImVec2 minimum, ImVec2 maximum, ImU32 colour,
		float rounding = 0.0f, ImDrawFlags flags = 0);
	void AddTriangleFilled(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 colour);
	void AddCircle(ImVec2 centre, float radius, ImU32 colour,
		int segments = 0, float thickness = 1.0f);
	void AddCircleFilled(ImVec2 centre, float radius, ImU32 colour, int segments = 0);
	void AddPolyline(ImVec2 const* points, int count, ImU32 colour,
		ImDrawFlags flags, float thickness);
	void AddText(ImVec2 position, ImU32 colour, char const* text);
	void AddText(ImFont* font, float fontSize, ImVec2 position, ImU32 colour,
		char const* text, char const* textEnd = nullptr, float wrapWidth = 0.0f,
		ImVec4 const* fineClipRect = nullptr);
	void AddImage(Texture texture, ImVec2 minimum, ImVec2 maximum,
		ImVec2 uvMinimum, ImVec2 uvMaximum, ImU32 colour = IM_COL32_WHITE);
	void AddDrawCmd();

	std::vector<Command> const& commands() const { return mCommands; }

private:
	ClipRectangle currentClip() const;
	void addTriangle(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 uvA, ImVec2 uvB,
		ImVec2 uvC, ImU32 colour, Texture texture);

	ImDrawList* mTestAdapter{};
	std::vector<ClipRectangle> mClipStack;
	std::vector<Command> mCommands;
};
