#include "WorldDrawList.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	WorldDrawList::ClipRectangle intersect(
		WorldDrawList::ClipRectangle const& left,
		WorldDrawList::ClipRectangle const& right)
	{
		return {
			{ std::max(left.minimum.x, right.minimum.x),
				std::max(left.minimum.y, right.minimum.y) },
			{ std::min(left.maximum.x, right.maximum.x),
				std::min(left.maximum.y, right.maximum.y) }
		};
	}

	int circleSegments(float radius, int requested)
	{
		if (requested > 2) return requested;
		return std::clamp(static_cast<int>(std::ceil(radius * 0.75f)), 12, 64);
	}
}

WorldDrawList::WorldDrawList(ClipRectangle initialClip)
{
	mClipStack.push_back(initialClip);
}

WorldDrawList::WorldDrawList(ImDrawList* testAdapter)
	: mTestAdapter(testAdapter)
{
	if (testAdapter)
	{
		mClipStack.push_back({ testAdapter->GetClipRectMin(), testAdapter->GetClipRectMax() });
	}
	else
	{
		float const limit = std::numeric_limits<float>::max();
		mClipStack.push_back({ { -limit, -limit }, { limit, limit } });
	}
}

WorldDrawList::ClipRectangle WorldDrawList::currentClip() const
{
	return mClipStack.back();
}

void WorldDrawList::PushClipRect(ImVec2 minimum, ImVec2 maximum,
	bool intersectWithCurrentClip)
{
	if (mTestAdapter) mTestAdapter->PushClipRect(minimum, maximum, intersectWithCurrentClip);
	ClipRectangle next{ minimum, maximum };
	if (intersectWithCurrentClip) next = intersect(currentClip(), next);
	mClipStack.push_back(next);
}

void WorldDrawList::PopClipRect()
{
	if (mClipStack.size() <= 1) return;
	if (mTestAdapter) mTestAdapter->PopClipRect();
	mClipStack.pop_back();
}

ImVec2 WorldDrawList::GetClipRectMin() const
{
	return currentClip().minimum;
}

ImVec2 WorldDrawList::GetClipRectMax() const
{
	return currentClip().maximum;
}

void WorldDrawList::AddLine(ImVec2 from, ImVec2 to, ImU32 colour, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddLine(from, to, colour, thickness);
		return;
	}
	mCommands.push_back(Line{ from, to, colour, thickness, currentClip() });
}

void WorldDrawList::AddRect(ImVec2 minimum, ImVec2 maximum, ImU32 colour,
	float rounding, ImDrawFlags flags, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddRect(minimum, maximum, colour, rounding, flags, thickness);
		return;
	}
	(void)rounding;
	(void)flags;
	AddLine(minimum, { maximum.x, minimum.y }, colour, thickness);
	AddLine({ maximum.x, minimum.y }, maximum, colour, thickness);
	AddLine(maximum, { minimum.x, maximum.y }, colour, thickness);
	AddLine({ minimum.x, maximum.y }, minimum, colour, thickness);
}

void WorldDrawList::addTriangle(ImVec2 a, ImVec2 b, ImVec2 c,
	ImVec2 uvA, ImVec2 uvB, ImVec2 uvC, ImU32 colour, Texture texture)
{
	Triangle triangle;
	triangle.positions[0] = a;
	triangle.positions[1] = b;
	triangle.positions[2] = c;
	triangle.texcoords[0] = uvA;
	triangle.texcoords[1] = uvB;
	triangle.texcoords[2] = uvC;
	triangle.colour = colour;
	triangle.texture = texture;
	triangle.clip = currentClip();
	mCommands.push_back(triangle);
}

void WorldDrawList::AddRectFilled(ImVec2 minimum, ImVec2 maximum, ImU32 colour,
	float rounding, ImDrawFlags flags)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddRectFilled(minimum, maximum, colour, rounding, flags);
		return;
	}
	(void)rounding;
	(void)flags;
	addTriangle(minimum, { maximum.x, minimum.y }, maximum, {}, {}, {}, colour, Texture::None);
	addTriangle(minimum, maximum, { minimum.x, maximum.y }, {}, {}, {}, colour, Texture::None);
}

void WorldDrawList::AddTriangleFilled(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 colour)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddTriangleFilled(a, b, c, colour);
		return;
	}
	addTriangle(a, b, c, {}, {}, {}, colour, Texture::None);
}

void WorldDrawList::AddCircle(ImVec2 centre, float radius, ImU32 colour,
	int segments, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddCircle(centre, radius, colour, segments, thickness);
		return;
	}
	auto const count = circleSegments(radius, segments);
	constexpr float tau = 6.28318530717958647692f;
	for (int index = 0; index < count; ++index)
	{
		auto point = [&](int i)
		{
			float const angle = tau * static_cast<float>(i % count) / static_cast<float>(count);
			return ImVec2{ centre.x + std::cos(angle) * radius,
				centre.y + std::sin(angle) * radius };
		};
		AddLine(point(index), point(index + 1), colour, thickness);
	}
}

void WorldDrawList::AddCircleFilled(ImVec2 centre, float radius, ImU32 colour, int segments)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddCircleFilled(centre, radius, colour, segments);
		return;
	}
	auto const count = circleSegments(radius, segments);
	constexpr float tau = 6.28318530717958647692f;
	for (int index = 0; index < count; ++index)
	{
		auto point = [&](int i)
		{
			float const angle = tau * static_cast<float>(i % count) / static_cast<float>(count);
			return ImVec2{ centre.x + std::cos(angle) * radius,
				centre.y + std::sin(angle) * radius };
		};
		AddTriangleFilled(centre, point(index), point(index + 1), colour);
	}
}

void WorldDrawList::AddPolyline(ImVec2 const* points, int count, ImU32 colour,
	ImDrawFlags flags, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddPolyline(points, count, colour, flags, thickness);
		return;
	}
	if (!points || count < 2) return;
	for (int index = 1; index < count; ++index)
		AddLine(points[index - 1], points[index], colour, thickness);
	if ((flags & ImDrawFlags_Closed) != 0)
		AddLine(points[count - 1], points[0], colour, thickness);
}

void WorldDrawList::AddText(ImVec2 position, ImU32 colour, char const* text)
{
	if (!text) return;
	if (mTestAdapter)
	{
		mTestAdapter->AddText(position, colour, text);
		return;
	}
	mCommands.push_back(Text{ position, colour, text, currentClip() });
}

void WorldDrawList::AddText(ImFont* font, float fontSize, ImVec2 position,
	ImU32 colour, char const* text, char const* textEnd, float wrapWidth,
	ImVec4 const* fineClipRect)
{
	if (!text) return;
	if (mTestAdapter)
	{
		mTestAdapter->AddText(font, fontSize, position, colour, text, textEnd,
			wrapWidth, fineClipRect);
		return;
	}
	(void)font;
	(void)fontSize;
	(void)wrapWidth;
	auto clip = currentClip();
	if (fineClipRect)
		clip = intersect(clip, { { fineClipRect->x, fineClipRect->y },
			{ fineClipRect->z, fineClipRect->w } });
	std::string value = textEnd ? std::string(text, textEnd) : std::string(text);
	mCommands.push_back(Text{ position, colour, std::move(value), clip });
}

void WorldDrawList::AddImage(Texture texture, ImVec2 minimum, ImVec2 maximum,
	ImVec2 uvMinimum, ImVec2 uvMaximum, ImU32 colour)
{
	if (mTestAdapter)
	{
		// The headless compatibility adapter has no production texture. A stable
		// non-null ID preserves ImDrawList's image geometry for legacy checks.
		auto id = reinterpret_cast<ImTextureID>(static_cast<intptr_t>(texture));
		mTestAdapter->AddImage(id, minimum, maximum, uvMinimum, uvMaximum, colour);
		return;
	}
	ImVec2 const topRight{ maximum.x, minimum.y };
	ImVec2 const bottomLeft{ minimum.x, maximum.y };
	ImVec2 const uvTopRight{ uvMaximum.x, uvMinimum.y };
	ImVec2 const uvBottomLeft{ uvMinimum.x, uvMaximum.y };
	addTriangle(minimum, topRight, maximum, uvMinimum, uvTopRight, uvMaximum,
		colour, texture);
	addTriangle(minimum, maximum, bottomLeft, uvMinimum, uvMaximum, uvBottomLeft,
		colour, texture);
}

void WorldDrawList::AddDrawCmd()
{
	if (mTestAdapter)
	{
		mTestAdapter->AddDrawCmd();
		return;
	}
	mCommands.push_back(Barrier{});
}
