#include "WorldRenderSystem.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <GL/glew.h>

#include <mpp/Camera.h>
#include <mpp/ClipRectangle.h>
#include <mpp/Colour.h>
#include <mpp/Logger.h>
#include <mpp/RenderSystem.h>
#include <mpp/RenderTexture.h>
#include <mpp/ResourceManager.h>
#include <mpp/Scene.h>
#include <mpp/helper/LineBatchDataProvider.h>
#include <mpp/helper/LineBatchRenderer.h>
#include <mpp/helper/TriangleBatchDataProvider.h>
#include <mpp/helper/TriangleBatchRenderer.h>
#include <mpp/mesh/VertexTypeSpecification.h>

#include <willpower/application/resourcesystem/DirectoryResourceLocation.h>
#include <willpower/application/resourcesystem/ImageResource.h>
#include <willpower/application/resourcesystem/ImageSetResource.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include <willpower/common/Logger.h>

#include "ApplicationResources.h"
#include "ObjectTileset.h"
#include "SectorTileset.h"
#include "WorldDrawList.h"

namespace resources = wp::application::resourcesystem;
namespace
{
	using Float = mpp::mesh::DataTypeFloat;
	using Byte = mpp::mesh::DataTypeUnsignedByte;

	bool same(ImVec2 left, ImVec2 right)
	{
		return left.x == right.x && left.y == right.y;
	}

	bool same(WorldDrawList::ClipRectangle const& left,
		WorldDrawList::ClipRectangle const& right)
	{
		return same(left.minimum, right.minimum) && same(left.maximum, right.maximum);
	}

	mpp::Colour colour(ImU32 value)
	{
		return {
			static_cast<float>((value >> IM_COL32_R_SHIFT) & 0xff) / 255.0f,
			static_cast<float>((value >> IM_COL32_G_SHIFT) & 0xff) / 255.0f,
			static_cast<float>((value >> IM_COL32_B_SHIFT) & 0xff) / 255.0f,
			static_cast<float>((value >> IM_COL32_A_SHIFT) & 0xff) / 255.0f
		};
	}

	void bytes(ImU32 value, uint8_t& red, uint8_t& green, uint8_t& blue,
		uint8_t& alpha)
	{
		red = static_cast<uint8_t>((value >> IM_COL32_R_SHIFT) & 0xff);
		green = static_cast<uint8_t>((value >> IM_COL32_G_SHIFT) & 0xff);
		blue = static_cast<uint8_t>((value >> IM_COL32_B_SHIFT) & 0xff);
		alpha = static_cast<uint8_t>((value >> IM_COL32_A_SHIFT) & 0xff);
	}

	void setNativeLineWidth(mpp::RenderSystem const& renderSystem, float width)
	{
		auto const& range = renderSystem.getCaps().aliasedLineWidthRange;
		glLineWidth(std::clamp(width, range[0], range[1]));
	}

	class TriangleProvider final
		: public mpp::helper::TriangleBatch2DDataProvider<Float, Float, Byte>
	{
	public:
		void set(std::vector<WorldDrawList::Triangle> triangles, ImVec2 origin)
		{
			mTriangles = std::move(triangles);
			mOrigin = origin;
			setNumPrimitives(mTriangles.size());
		}

		void position(uint32_t, uint32_t index, float& x0, float& y0,
			float& x1, float& y1, float& x2, float& y2) override
		{
			auto const& triangle = mTriangles.at(index);
			auto transform = [this](ImVec2 point)
			{
				// MPP's 2D vertex shader converts top-left-origin screen Y to
				// OpenGL coordinates. Supplying an already-inverted Y flips the
				// complete canvas a second time.
				return ImVec2{ point.x - mOrigin.x, point.y - mOrigin.y };
			};
			auto const a = transform(triangle.positions[0]);
			auto const b = transform(triangle.positions[1]);
			auto const c = transform(triangle.positions[2]);
			x0 = a.x; y0 = a.y;
			x1 = b.x; y1 = b.y;
			x2 = c.x; y2 = c.y;
		}

		void texcoords(uint32_t, uint32_t index, float& u0, float& v0,
			float& u1, float& v1, float& u2, float& v2) override
		{
			auto const& uv = mTriangles.at(index).texcoords;
			u0 = uv[0].x; v0 = 1.0f - uv[0].y;
			u1 = uv[1].x; v1 = 1.0f - uv[1].y;
			u2 = uv[2].x; v2 = 1.0f - uv[2].y;
		}

		void colour(uint32_t, uint32_t index, uint8_t& red, uint8_t& green,
			uint8_t& blue, uint8_t& alpha) override
		{
			bytes(mTriangles.at(index).colour, red, green, blue, alpha);
		}

		mpp::Colour diffuse(uint32_t) override { return mpp::Colour::White; }

		void getBounds(glm::vec3& minimum, glm::vec3& maximum) override
		{
			minimum = { 0.0f, 0.0f, 0.0f };
			maximum = { 0.0f, 0.0f, 0.0f };
		}

	private:
		std::vector<WorldDrawList::Triangle> mTriangles;
		ImVec2 mOrigin{};
	};

	class LineProvider final
		: public mpp::helper::LineBatchDataProvider<Float, Byte>
	{
	public:
		void set(std::vector<WorldDrawList::Line> lines, ImVec2 origin)
		{
			mLines = std::move(lines);
			mOrigin = origin;
			setNumPrimitives(mLines.size());
		}

		void position(uint32_t index, float& x0, float& y0,
			float& x1, float& y1) override
		{
			auto const& line = mLines.at(index);
			x0 = line.from.x - mOrigin.x;
			y0 = line.from.y - mOrigin.y;
			x1 = line.to.x - mOrigin.x;
			y1 = line.to.y - mOrigin.y;
		}

		void colour(uint32_t index, uint8_t& red, uint8_t& green,
			uint8_t& blue, uint8_t& alpha) override
		{
			bytes(mLines.at(index).colour, red, green, blue, alpha);
		}

		mpp::Colour diffuse() override { return mpp::Colour::White; }

		void getBounds(glm::vec3& minimum, glm::vec3& maximum) override
		{
			minimum = { 0.0f, 0.0f, 0.0f };
			maximum = { 0.0f, 0.0f, 0.0f };
		}

	private:
		std::vector<WorldDrawList::Line> mLines;
		ImVec2 mOrigin{};
	};

	struct Segment
	{
		enum class Kind { Triangles, Lines, Text } kind{};
		WorldDrawList::Texture texture{ WorldDrawList::Texture::None };
		float lineWidth{ 1.0f };
		WorldDrawList::ClipRectangle clip{};
		std::vector<WorldDrawList::Triangle> triangles;
		std::vector<WorldDrawList::Line> lines;
		std::vector<WorldDrawList::Text> texts;
	};

	class WorldRenderSystem
	{
		struct Slot
		{
			Segment::Kind kind{};
			WorldDrawList::Texture texture{ WorldDrawList::Texture::None };
			std::shared_ptr<TriangleProvider> triangles;
			std::shared_ptr<LineProvider> lines;
			std::shared_ptr<mpp::helper::TriangleBatch2DRenderer<Float, Float, Byte>> triangleRenderer;
			std::shared_ptr<mpp::helper::LineBatchRenderer<Float, Byte>> lineRenderer;
			mpp::SceneModel2dPtr sceneModel;
		};

	public:
		WorldRenderSystem(std::filesystem::path const& resourceDirectory,
			std::size_t width, std::size_t height)
		{
			mMppLogger = std::make_unique<mpp::Logger>();
			if (!mMppLogger->initialise("prometheum-fermide-mpp.log",
				mpp::Logger::Level::Debug))
				throw std::runtime_error("Could not initialise the MPP logger");
			mWpLogger = std::make_unique<wp::Logger>();
			mWpLogger->open("prometheum-fermide-resources.html");

			mRenderSystem = std::make_unique<mpp::RenderSystem>(width, height,
				mMppLogger.get());
			mRenderResources = std::make_unique<mpp::ResourceManager>(
				mRenderSystem.get(), mMppLogger.get());
			mRenderSystem->createCoreResources(mRenderResources.get());
			mResources = std::make_unique<resources::ResourceManager>(
				mRenderSystem.get(), mRenderResources.get(), nullptr, mWpLogger.get());
			registerApplicationResourceTypes(*mResources);
			mResources->addResourceLocationFactory("Directory",
				[logger = mWpLogger.get()](std::string const& location,
					std::string const& definition)
				{
					return new resources::DirectoryResourceLocation(
						logger, location, definition);
				});
			mResources->addResourceLocation("Directory", resourceDirectory.string(),
				"Resources.yaml");
			mResources->scanLocations();

			mSectorSet = requireImageSet("SectorAtlas");
			mObjectSet = requireImageSet("ObjectAtlas");
			installAtlasDefinitions();

			mScene = mRenderSystem->createScene("Default");
			mScene->load();
			mScene->setClearColour({ 0.08f, 0.08f, 0.08f, 1.0f });
		}

		~WorldRenderSystem()
		{
			for (auto& slot : mSlots) remove(slot);
			mSlots.clear();
			if (mScene)
			{
				mScene->unload();
				mScene.reset();
			}
			mTarget.reset();
			if (mSectorSet) mResources->releaseResource(mSectorSet);
			if (mObjectSet) mResources->releaseResource(mObjectSet);
			clearSectorTileset();
			clearObjectTileset();
			mResources.reset();
			if (mRenderSystem) mRenderSystem->destroyCoreResources();
			mRenderResources.reset();
			mRenderSystem.reset();
			mWpLogger.reset();
			mMppLogger.reset();
		}

		uint32_t render(WorldDrawList const& commandList, ImVec2 origin, ImVec2 size)
		{
			auto const width = static_cast<std::size_t>(std::max(1.0f, std::ceil(size.x)));
			auto const height = static_cast<std::size_t>(std::max(1.0f, std::ceil(size.y)));
			ensureTarget(width, height);
			auto segments = buildSegments(commandList);
			prepareSlots(segments, origin);

			mRenderSystem->pushRenderTarget(mTarget);
			mRenderSystem->setViewport(0, 0, width, height);
			mRenderSystem->clearScreen(mScene->getClearColour());
			mRenderSystem->setProjection2dOrthographic();
			mRenderSystem->resetTransform();

			for (std::size_t index = 0; index < segments.size(); ++index)
			{
				auto const& segment = segments[index];
				auto const clip = targetClip(segment.clip, origin,
					static_cast<int>(width), static_cast<int>(height));
				// MPP treats an empty scissor as no active clip. Do not submit
				// commands whose CPU-side clip intersection has no area; a closed
				// Door's zero-height aperture must not repaint over its leaf.
				if (clip.width == 0 || clip.height == 0) continue;
				mRenderSystem->pushClipRectangle(clip);
				if (segment.kind == Segment::Kind::Text)
				{
					for (auto const& text : segment.texts)
						mRenderSystem->renderText(text.value,
							static_cast<int>(text.position.x - origin.x),
							static_cast<int>(text.position.y - origin.y),
							colour(text.colour));
				}
				else
				{
					if (segment.kind == Segment::Kind::Lines)
						setNativeLineWidth(*mRenderSystem, segment.lineWidth);
					mSlots[index].sceneModel->render({});
					mRenderSystem->flushVertexBuffers();
				}
				mRenderSystem->popClipRectangle();
			}
			setNativeLineWidth(*mRenderSystem, 1.0f);
			mRenderSystem->popRenderTarget();

			return static_cast<mpp::RenderTexture*>(mTarget.get())->getId();
		}

	private:
		resources::ResourcePtr requireImageSet(std::string const& name)
		{
			auto resource = mResources->acquireResource(name);
			try
			{
				mResources->createResource(resource);
				mResources->loadResource(resource);
			}
			catch (...)
			{
				mResources->releaseResource(resource);
				throw;
			}
			if (!std::dynamic_pointer_cast<resources::ImageSetResource>(resource))
				throw std::runtime_error(name + " is not an ImageSet resource");
			return resource;
		}

		void installAtlasDefinitions()
		{
			auto sector = std::dynamic_pointer_cast<resources::ImageSetResource>(mSectorSet);
			auto sectorImage = std::dynamic_pointer_cast<resources::ImageResource>(sector->getImage());
			SectorTileset sectorTiles;
			sectorTiles.width = sectorImage->getWidth();
			sectorTiles.height = sectorImage->getHeight();
			for (auto const& [name, definition] : sector->getImageDefinitions())
			{
				SectorTileRegion region{ static_cast<int>(definition.x),
					static_cast<int>(definition.y), static_cast<int>(definition.width),
					static_cast<int>(definition.height) };
				if (name.rfind("boundary-", 0) == 0)
					sectorTiles.boundaries.emplace(name.substr(9), region);
				else sectorTiles.surfaces.emplace(name, region);
			}
			setSectorTileset(std::move(sectorTiles), reinterpret_cast<ImTextureID>(1));

			auto objects = std::dynamic_pointer_cast<resources::ImageSetResource>(mObjectSet);
			auto objectImage = std::dynamic_pointer_cast<resources::ImageResource>(objects->getImage());
			ObjectTileset objectTiles;
			objectTiles.width = objectImage->getWidth();
			objectTiles.height = objectImage->getHeight();
			for (auto const& [name, definition] : objects->getImageDefinitions())
			{
				SectorTileRegion region{ static_cast<int>(definition.x),
					static_cast<int>(definition.y), static_cast<int>(definition.width),
					static_cast<int>(definition.height) };
				auto const tintable = name == "agent" || name == "marker";
				objectTiles.sprites.emplace(name, ObjectSprite{ region, tintable });
			}
			setObjectTileset(std::move(objectTiles), reinterpret_cast<ImTextureID>(1));
		}

		mpp::ResourcePtr texture(WorldDrawList::Texture textureKind) const
		{
			if (textureKind == WorldDrawList::Texture::None) return {};
			auto set = std::dynamic_pointer_cast<resources::ImageSetResource>(
				textureKind == WorldDrawList::Texture::SectorAtlas ? mSectorSet : mObjectSet);
			return set->getImage()->getMppResource();
		}

		static std::vector<Segment> buildSegments(WorldDrawList const& commandList)
		{
			std::vector<Segment> result;
			for (auto const& command : commandList.commands())
			{
				if (std::holds_alternative<WorldDrawList::Barrier>(command))
				{
					if (!result.empty()) result.push_back({});
					continue;
				}
				if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command))
				{
					bool const append = !result.empty()
						&& result.back().kind == Segment::Kind::Triangles
						&& result.back().texture == triangle->texture
						&& same(result.back().clip, triangle->clip);
					if (!append)
					{
						Segment segment;
						segment.kind = Segment::Kind::Triangles;
						segment.texture = triangle->texture;
						segment.clip = triangle->clip;
						result.push_back(std::move(segment));
					}
					result.back().triangles.push_back(*triangle);
				}
				else if (auto const* line = std::get_if<WorldDrawList::Line>(&command))
				{
					bool const append = !result.empty()
						&& result.back().kind == Segment::Kind::Lines
						&& result.back().lineWidth == line->thickness
						&& same(result.back().clip, line->clip);
					if (!append)
					{
						Segment segment;
						segment.kind = Segment::Kind::Lines;
						segment.lineWidth = line->thickness;
						segment.clip = line->clip;
						result.push_back(std::move(segment));
					}
					result.back().lines.push_back(*line);
				}
				else if (auto const* text = std::get_if<WorldDrawList::Text>(&command))
				{
					bool const append = !result.empty()
						&& result.back().kind == Segment::Kind::Text
						&& same(result.back().clip, text->clip);
					if (!append)
					{
						Segment segment;
						segment.kind = Segment::Kind::Text;
						segment.clip = text->clip;
						result.push_back(std::move(segment));
					}
					result.back().texts.push_back(*text);
				}
			}
			std::erase_if(result, [](Segment const& segment)
				{ return segment.triangles.empty() && segment.lines.empty()
					&& segment.texts.empty(); });
			return result;
		}

		void prepareSlots(std::vector<Segment> const& segments, ImVec2 origin)
		{
			for (std::size_t index = 0; index < segments.size(); ++index)
			{
				if (index >= mSlots.size()) mSlots.emplace_back();
				auto& slot = mSlots[index];
				auto const& segment = segments[index];
				if (segment.kind == Segment::Kind::Text)
				{
					remove(slot);
					slot.kind = Segment::Kind::Text;
					continue;
				}
				if ((!slot.triangleRenderer && !slot.lineRenderer)
					|| slot.kind != segment.kind || slot.texture != segment.texture)
				{
					remove(slot);
					slot.kind = segment.kind;
					slot.texture = segment.texture;
					create(slot, index);
				}
				if (slot.triangles)
				{
					slot.triangles->set(segment.triangles, origin);
					slot.triangleRenderer->update();
					slot.sceneModel->setVisible(true);
				}
				else
				{
					slot.lines->set(segment.lines, origin);
					slot.lineRenderer->update();
					slot.sceneModel->setVisible(true);
				}
			}
			for (std::size_t index = segments.size(); index < mSlots.size(); ++index)
			{
				if (mSlots[index].sceneModel) mSlots[index].sceneModel->setVisible(false);
			}
		}

		void create(Slot& slot, std::size_t order)
		{
			auto const name = "WorldCanvas." + std::to_string(order);
			if (slot.kind == Segment::Kind::Triangles)
			{
				slot.triangles = std::make_shared<TriangleProvider>();
				mpp::helper::TriangleBatchRendererParams options(
					false, false, false, false, false);
				slot.triangleRenderer = std::make_shared<
					mpp::helper::TriangleBatch2DRenderer<Float, Float, Byte>>(
						name, options, slot.triangles, texture(slot.texture),
						mRenderSystem.get(), mRenderResources.get());
				slot.triangleRenderer->create();
				slot.sceneModel = mScene->add2dBatch(
					slot.triangles, slot.triangleRenderer, static_cast<int>(order));
				slot.sceneModel->getParams()->setModelBlend(true);
			}
			else
			{
				slot.lines = std::make_shared<LineProvider>();
				mpp::helper::LineBatchRendererParams options{ false, true, false };
				slot.lineRenderer = std::make_shared<
					mpp::helper::LineBatchRenderer<Float, Byte>>(
						name, options, slot.lines, mRenderSystem.get(),
						mRenderResources.get());
				slot.lineRenderer->create();
				slot.sceneModel = mScene->add2dBatch(
					slot.lines, slot.lineRenderer, static_cast<int>(order));
				slot.sceneModel->getParams()->setModelBlend(true);
			}
		}

		void remove(Slot& slot)
		{
			if (slot.sceneModel)
			{
				mScene->remove2dBatch(slot.sceneModel);
				slot.sceneModel.reset();
			}
			slot.triangleRenderer.reset();
			slot.lineRenderer.reset();
			slot.triangles.reset();
			slot.lines.reset();
		}

		void ensureTarget(std::size_t width, std::size_t height)
		{
			if (!mTarget)
				mTarget = mRenderSystem->createRenderTexture(
					"WorldCanvas.Target", width, height, 1, false);
			else if (mTarget->getWidth() != width || mTarget->getHeight() != height)
				static_cast<mpp::RenderTexture*>(mTarget.get())->resize(width, height);
			mScene->setViewport(0, 0, width, height);
		}

		static mpp::ClipRectangle targetClip(WorldDrawList::ClipRectangle clip,
			ImVec2 origin, int width, int height)
		{
			int const left = std::clamp(static_cast<int>(std::floor(
				clip.minimum.x - origin.x)), 0, width);
			int const right = std::clamp(static_cast<int>(std::ceil(
				clip.maximum.x - origin.x)), 0, width);
			int const top = std::clamp(static_cast<int>(std::floor(
				clip.minimum.y - origin.y)), 0, height);
			int const bottom = std::clamp(static_cast<int>(std::ceil(
				clip.maximum.y - origin.y)), 0, height);
			return { left, height - bottom, std::max(0, right - left),
				std::max(0, bottom - top) };
		}

		std::unique_ptr<mpp::Logger> mMppLogger;
		std::unique_ptr<wp::Logger> mWpLogger;
		std::unique_ptr<mpp::RenderSystem> mRenderSystem;
		std::unique_ptr<mpp::ResourceManager> mRenderResources;
		std::unique_ptr<resources::ResourceManager> mResources;
		resources::ResourcePtr mSectorSet;
		resources::ResourcePtr mObjectSet;
		mpp::ScenePtr mScene;
		mpp::RenderTargetPtr mTarget;
		std::vector<Slot> mSlots;
	};

	std::unique_ptr<WorldRenderSystem> gSystem;
}

void createWorldRenderSystem(std::filesystem::path const& resourceDirectory,
	std::size_t drawableWidth, std::size_t drawableHeight)
{
	if (gSystem) throw std::logic_error("The World render system already exists");
	gSystem = std::make_unique<WorldRenderSystem>(resourceDirectory,
		drawableWidth, drawableHeight);
}

void resizeWorldRenderSystem(std::size_t, std::size_t)
{
	// Offscreen targets follow the canvas. MPP's window dimensions are used only
	// by presentation pipelines, which this editor-owned renderer does not use.
}

void destroyWorldRenderSystem()
{
	gSystem.reset();
}

std::uint32_t renderWorldCommands(WorldDrawList const& commands,
	ImVec2 canvasPosition, ImVec2 canvasSize)
{
	if (!gSystem) throw std::runtime_error("The World render system is not initialised");
	return gSystem->render(commands, canvasPosition, canvasSize);
}
