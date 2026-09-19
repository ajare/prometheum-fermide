#define NOMINMAX

#if defined(_WIN32)
#include <Windows.h>
#include <GL/glew.h>
#include <nfd.h>
#elif defined(__linux__)
#include <GL/glew.h>
#include <nfd.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <stdexcept>
#else
#error "Unsupported platform"
#endif

#include <filesystem>
#include <fstream>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4307)
#endif
#include <spdlog/spdlog.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/details/log_msg_buffer.h>

#if defined(_WIN32)
#include <SDL2/SDL.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL2/SDL_opengles2.h>
#else
#include <SDL2/SDL_opengl.h>
#endif
#elif defined(__linux__)
#include <SDL.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL_opengles2.h>
#else
#include <SDL_opengl.h>
#endif
#else
#error "Unsupported platform"
#endif

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_sdl2.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imnodes.h"
#include "imgui/IconsFontAwesome5.h"

#include "core/Building.h"
#include "core/Pathing.h"
#include "core/Exceptions.h"
#include "core/Log.h"

#include "Main.h"
#include "Render.h"
#include "Helpers.h"
#include "UI.h"
#include "UISettings.h"
#include "Exceptions.h"


spdlog::logger* gLogger{ nullptr };
SDL_Window* gWindow{ nullptr };
SDL_GLContext gContext{ nullptr };
UISettings gUISettings;

ImFont* gAgentIconFont{ nullptr };
std::filesystem::path gResourceDirectory;

namespace
{
	// Tracks which subsystems actually finished initialising so shutdown can
	// release only what exists after a partial-startup failure (#59).
	bool gSdlInitialised{ false };
	bool gImGuiContextCreated{ false };
	bool gImGuiSdlBackendInitialised{ false };
	bool gImGuiOpenGLBackendInitialised{ false };
	bool gNfdInitialised{ false };
}

using namespace std;

namespace
{
	string trim(string value)
	{
		auto const first = value.find_first_not_of(" \t\r\n");
		if (first == string::npos) return {};
		auto const last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}

	filesystem::path executableDirectory()
	{
#if defined(_WIN32)
		wstring executablePath(MAX_PATH, L'\0');
		auto const length = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
		if (length == 0 || length == executablePath.size())
		{
			throw ExitApplicationException(1, "Could not determine the executable directory.");
		}
		executablePath.resize(length);
		return filesystem::path(executablePath).parent_path();
#elif defined(__linux__)
		array<char, 4096> executablePath{};
		auto const length = readlink("/proc/self/exe", executablePath.data(), executablePath.size() - 1);
		if (length <= 0 || static_cast<size_t>(length) >= executablePath.size() - 1)
		{
			throw ExitApplicationException(1, "Could not determine the executable directory.");
		}
		return filesystem::path(string(executablePath.data(), static_cast<size_t>(length))).parent_path();
#else
#error "Unsupported platform"
#endif
	}

	filesystem::path loadResourceDirectory()
	{
		auto const configurationPath = executableDirectory() / "prometheum-fermide.ini";
		ifstream configuration(configurationPath);
		if (!configuration)
		{
			throw ExitApplicationException(1, "Could not open ImGui configuration: " + configurationPath.string());
		}

		string section;
		string resourceDirectory;
		for (string line; getline(configuration, line);)
		{
			line = trim(line);
			if (line.empty() || line.starts_with(';') || line.starts_with('#')) continue;
			if (line.front() == '[' && line.back() == ']')
			{
				section = trim(line.substr(1, line.size() - 2));
				continue;
			}

			auto const separator = line.find('=');
			if (section == "Config" && separator != string::npos
				&& trim(line.substr(0, separator)) == "ResourceDir")
			{
				resourceDirectory = trim(line.substr(separator + 1));
			}
		}

		if (resourceDirectory.empty())
		{
			throw ExitApplicationException(1, "ImGui configuration does not define Config.ResourceDir.");
		}

		filesystem::path resourcePath(resourceDirectory);
		if (resourcePath.is_relative()) resourcePath = executableDirectory() / resourcePath;
		resourcePath = resourcePath.lexically_normal();
		if (!filesystem::is_directory(resourcePath))
		{
			throw ExitApplicationException(1, "Configured resource directory does not exist: " + resourcePath.string());
		}
		return resourcePath;
	}
}

SDL_Window* createWindow()
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
	{
		throw ExitApplicationException(1, "SDL initialisation failed: " + string(SDL_GetError()));
	}
	gSdlInitialised = true;

	// GL 3.0 + GLSL 130
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

	// From 2.0.18: Enable native IME.
#ifdef SDL_HINT_IME_SHOW_UI
	SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif

	// Create window with graphics context
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	SDL_Window* window = SDL_CreateWindow("ImGui PF Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, window_flags);
	if (window == nullptr)
	{
		throw ExitApplicationException(1, "SDL window creation failed: " + string(SDL_GetError()));
	}

	gLogger->info("Window created");

	return window;
}

SDL_GLContext createContext(SDL_Window* window)
{
	SDL_GLContext gl_context = SDL_GL_CreateContext(window);
	if (gl_context == nullptr)
	{
		throw ExitApplicationException(1, "OpenGL context creation failed: " + string(SDL_GetError()));
	}

	if (SDL_GL_MakeCurrent(window, gl_context) != 0)
	{
		SDL_GL_DeleteContext(gl_context);
		throw ExitApplicationException(1, "Could not make the OpenGL context current: " + string(SDL_GetError()));
	}

	if (SDL_GL_SetSwapInterval(1) != 0) // Enable vsync
	{
		gLogger->warn("Could not enable vsync: " + string(SDL_GetError()));
	}

	gLogger->info("OpenGL context created");

	return gl_context;
}

void setupImGui(SDL_Window* window, SDL_GLContext context)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	gImGuiContextCreated = true;
	ImNodes::CreateContext();
	ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkDetachWithDragClick);

	// Setup Dear ImGui style
	switch (gUISettings.style)
	{
	case UISettings::Light:
		ImGui::StyleColorsLight();
		break;

	case UISettings::Dark:
		ImGui::StyleColorsDark();
		break;

	case UISettings::Classic:
		ImGui::StyleColorsClassic();
		break;
	}

	// Setup Platform/Renderer backends
	ImGui_ImplSDL2_InitForOpenGL(window, context);
	gImGuiSdlBackendInitialised = true;

	const char* glsl_version = "#version 130";
	if (!ImGui_ImplOpenGL3_Init(glsl_version))
	{
		throw ExitApplicationException(1, "ImGui OpenGL3 backend initialisation failed; the GLSL version is unsupported.");
	}
	gImGuiOpenGLBackendInitialised = true;

	// Load Fonts
	// - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
	// - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
	// - If the file cannot be loaded, the function will return NULL. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
	// - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
	// - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
	// - Read 'docs/FONTS.md' for more instructions and details.
	// - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
	// - Our Emscripten build process allows embedding fonts to be accessible at runtime from the "fonts/" folder. See Makefile.emscripten for details.
	//io.Fonts->AddFontDefault();
	//io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
	//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
	//ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
	//IM_ASSERT(font != NULL);

	gLogger->info("ImGui set up");
}

auto callbackSink = std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg& msg)
{
	core::LogLevel level;
	switch (msg.level)
	{
	case spdlog::level::trace:
	case spdlog::level::debug:
		level = core::LogLevel::Debug;
		break;

	case spdlog::level::info:
		level = core::LogLevel::Info;
		break;

	case spdlog::level::warn:
		level = core::LogLevel::Warning;
		break;

	case spdlog::level::err:
	case spdlog::level::critical:
	default:
		level = core::LogLevel::Error;
		break;
	}

	core::addLogMessage("Application", ~0u, level,
		string(msg.payload.data(), msg.payload.size()));
});

void setupLogging()
{
#ifdef _DEBUG
	filesystem::path const logPath("../../../logs/pf-debug.log");
#else
	filesystem::path const logPath("../../../logs/pf-release.log");
#endif

	// A missing log directory must not take the whole application down before
	// the startup handler is in place; create it if possible and let spdlog
	// report a genuinely unwritable destination as a controlled error.
	std::error_code ec;
	filesystem::create_directories(logPath.parent_path(), ec);

	auto fileSink = make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);

#ifdef _DEBUG
	auto consoleSink = make_shared<spdlog::sinks::stdout_color_sink_mt>();
	consoleSink->set_level(spdlog::level::debug);

	fileSink->set_level(spdlog::level::debug);

	gLogger = new spdlog::logger("pf", { consoleSink, fileSink, callbackSink });
#else
	fileSink->set_level(spdlog::level::info);

	gLogger = new spdlog::logger("editor", { fileSink, callbackSink });
#endif

	gLogger->set_level(spdlog::level::debug);
}

void initialise()
{
	setupLogging();

	//
	// Set up SDL
	//
	gWindow = createWindow();
	gContext = createContext(gWindow);

	glewExperimental = GL_TRUE;
	if (glewInit() != GLEW_OK)
	{
#if defined(_WIN32)
		throw exception("GLEW initialisation failed");
#elif defined(__linux__)
		throw runtime_error("GLEW initialisation failed");
#else
#error "Unsupported platform"
#endif
	}

	//
	// Set up ImGui
	//
	setupImGui(gWindow, gContext);
}

void setup()
{
	gResourceDirectory = loadResourceDirectory();
	initializeRecentFiles(executableDirectory() / "recent-files.txt");

	// Set up NFD (file dialogs)
	if (NFD_Init() != NFD_OKAY)
	{
		throw ExitApplicationException(1, "Native file dialog initialisation failed: " + string(NFD_GetError()));
	}
	gNfdInitialised = true;

	// ImGui extra twiddling
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable the docking branch UI

	io.Fonts->AddFontDefault();

	float baseFontSize = 13.0f; // 13.0f is the size of the default font. Change to the font size you use.
	float iconFontSize = baseFontSize * 2.0f / 3.0f; // FontAwesome fonts need to have their sizes reduced by 2.0f/3.0f in order to align correctly

	static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
	ImFontConfig icons_config;
	icons_config.MergeMode = true;
	icons_config.PixelSnapH = true;
	icons_config.GlyphMinAdvanceX = iconFontSize;
	auto const iconFontPath = (gResourceDirectory / FONT_ICON_FILE_NAME_FAS).string();
	io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), iconFontSize, &icons_config, icons_ranges);

	// Palette items and world entities are rendered much larger than toolbar icons.
	// Rasterize their glyphs at Agent height instead of enlarging the merged bitmap.
	static const ImWchar agentIconRanges[] = {
		0xf183, 0xf183, // Male
		0xf21d, 0xf21d, // Street View
		0xf2d0, 0xf2d0, // Window Maximize
		0xf3c5, 0xf3c5, // Map Marker Alt
		0xf52a, 0xf52a, // Door Closed
		0xf7a4, 0xf7a4, // Grip Lines (Walkway)
		0
	};
	gAgentIconFont = io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(),
		CORE_AGENT_MAX_HEIGHT * CORE_DECK_HEIGHT_PIXELS, nullptr, agentIconRanges);
	if (!gAgentIconFont)
	{
		throw ExitApplicationException(1, "Could not load the Agent icon font.");
	}

	io.Fonts->Build();
	if (!gAgentIconFont->FindGlyphNoFallback(0xf7a4))
	{
		throw ExitApplicationException(1, "Could not load the Walkway palette icon.");
	}
}

void shutdown()
{
	if (gLogger)
	{
		gLogger->info("Shutting down");
	}

	// ImGui backends, newest initialised first; skip anything that never came up.
	if (gImGuiOpenGLBackendInitialised)
	{
		ImGui_ImplOpenGL3_Shutdown();
		gImGuiOpenGLBackendInitialised = false;
	}

	if (gImGuiSdlBackendInitialised)
	{
		ImGui_ImplSDL2_Shutdown();
		gImGuiSdlBackendInitialised = false;
	}

	if (gImGuiContextCreated)
	{
		ImGui::DestroyContext();
		gImGuiContextCreated = false;
	}

	// Platform
	if (gContext != nullptr)
	{
		SDL_GL_DeleteContext(gContext);
		gContext = nullptr;
	}

	if (gWindow != nullptr)
	{
		SDL_DestroyWindow(gWindow);
		gWindow = nullptr;
	}

	if (gSdlInitialised)
	{
		SDL_Quit();
		gSdlInitialised = false;
	}

	if (gNfdInitialised)
	{
		NFD_Quit();
		gNfdInitialised = false;
	}

	delete gLogger;
	gLogger = nullptr;
}

bool processEvents(SDL_Window* window)
{
	// Poll and handle events (inputs, window resize, etc.)
	// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
	// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
	// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
	// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
	bool done = false;
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		ImGui_ImplSDL2_ProcessEvent(&event);
		if (event.type == SDL_QUIT)
		{
			done = true;
		}
		if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
		{
			done = true;
		}
	}

	return done;
}


std::shared_ptr<core::Building> createTestBuilding()
{
	auto building = make_shared<core::Building>("Citadel", 48, 6);

	try
	{
		enum CitadelDeck
		{
			ReactorDeck,
			HospitalDeck,
			MaintenanceDeck,
			StorageDeck,
			FlightDeck,
			ExecutiveDeck,
			EngineeringDeck,
			SecurityDeck,
			BridgeDeck
		};

		//
		// Reactor
		// 

		// First corridor
		building->addCorridor(ReactorDeck, 1, 8);
		
		building->addRoom("Pump Room", 1, ReactorDeck, 0, 4, 1);
		
		//building->addSectorLightSwitch(pumpRoomIndex, 0);

		building->addRoom("Fuel Cells", 1, ReactorDeck, 5, 3, 1);
		
		building->addSectorDoor(0, ReactorDeck, 6);
		
		building->addSectorDoor(0, ReactorDeck, 2);
		/*
		building->addSectorWindow(0, ReactorDeck, 7, 1, 1);
		
		// Second corridor
		auto corr2Index = building->addCorridor(ReactorDeck, 9, 6);

		// Reactor core
		auto reactorCoreIndex = building->addRoom("Reactor Core", 1, ReactorDeck, 9, 4, 3);
		
		building->addSectorWalkway(reactorCoreIndex, 1, 0);
		building->addSectorWalkway(reactorCoreIndex, 1, 2);
		building->addSectorWalkway(reactorCoreIndex, 1, 3);
		building->addSectorWalkway(reactorCoreIndex, 2, 0);
		building->addSectorWalkway(reactorCoreIndex, 2, 3);
		
		building->addSectorForceBridge(reactorCoreIndex, 1, 1, { 1, CORE_SIDE_RIGHT, true, true, 2 });
		
		building->addSectorLadder(reactorCoreIndex, 0, 0, { 2, true, true });
		building->addSectorLadder(reactorCoreIndex, 1, 3, { 2, true, true });

		building->addSectorMarker(reactorCoreIndex, 0, 3.0f);
		
		building->addSectorDoor(ReactorDeck, 10);

		building->addSectorWindow(0, ReactorDeck, 1, 1, 1);
		
		// Decontamination
		building->addRoom("Decontamination", 1, ReactorDeck, 14, 3, 1);
		building->addSectorDoor(ReactorDeck, 14);

		building->addSectorWindow(0, ReactorDeck, 11, 2, 1);

		// Connect corridors
		building->addSectorBulkheadDoor(0, ReactorDeck, 9, CORE_SIDE_LEFT);
		
		// Third corridor
		building->addCorridor(ReactorDeck, 16, 14);

		building->addSectorDoor(ReactorDeck, 16);

		// Fourth corridor
		building->addCorridor(ReactorDeck, 34, 9);

		building->addSectorDoor(ReactorDeck, 38);

		building->addShuttle(ReactorDeck, 24, 17, { 2, 3, { 0, 10 }, 0 });
*/
/*
		//
		// Hospital
		//
		building->addCorridor(HospitalDeck, 1, 4);
		building->addCorridor(HospitalDeck, 6, 10);
		auto corrIndex = building->addCorridor(HospitalDeck, 17, 4, 2);

		building->addSectorWalkway(corrIndex, 1, 1);
		building->addSectorWalkway(corrIndex, 1, 2);
		building->addSectorWalkway(corrIndex, 1, 3);

		auto icuIndex = building->addRoom("ICU", 1, HospitalDeck, 13, 3, 1, CORE_DOOR_HEIGHT + 0.1f);
		building->addSectorDoor(HospitalDeck, 14);

		building->removeLocationWall(icuIndex, 0, CORE_SIDE_LEFT);

		auto acIndex = building->addRoom("Autoclaves", 1, HospitalDeck, 0, 3, 4, CORE_DOOR_HEIGHT + 0.1f);

		building->addSectorWalkway(acIndex, 1, 0);
		building->addSectorWalkway(acIndex, 1, 1);
		building->addSectorWalkway(acIndex, 2, 0);
		building->addSectorWalkway(acIndex, 2, 1);

		building->addSectorPlatformLift(acIndex, 0, 0, { 1, { 0, 1, 2 } });

		building->addSectorDoor(HospitalDeck, 2);
		
		auto morgueCorrIndex = building->addCorridor(HospitalDeck, 22, 9);
		auto morgueIndex = building->addRoom("Morgue", 1, HospitalDeck, 22, 9, 1);
		building->addSectorDoor(HospitalDeck, 26);

		uint32_t vertexIdentifiers[20];
		for (int i = 0; i < 9; ++i)
		{
			if (i == 4) continue;

			uint32_t vertexIdentifier;
			building->addSectorMarker(morgueCorrIndex, 0, i + 0.5f, &vertexIdentifier);
			vertexIdentifiers[i * 2] = vertexIdentifier;

			building->addSectorMarker(morgueIndex, 0, i + 0.5f, &vertexIdentifier);
			vertexIdentifiers[i * 2 + 1] = vertexIdentifier;
		}

		uint32_t vertexIdentifier;
		building->addSectorMarker(reactorCorr1, 0, 3.0f, &vertexIdentifier);
		vertexIdentifiers[18] = vertexIdentifier;
		building->addSectorMarker(pumpRoomIndex, 0, 1.5f, &vertexIdentifier);
		vertexIdentifiers[19] = vertexIdentifier;


		//
		// Maintenance
		//
		building->addCorridor(MaintenanceDeck, 2, 3);
		building->addCorridor(MaintenanceDeck, 6, 3);

		//
		// Storage
		//
		auto storageCorrIndex = building->addCorridor(StorageDeck, 6, 6);
		building->addCorridor(StorageDeck, 15, 6);

		building->addShuttle(StorageDeck, 10, 7, { 1, 3, { 0, 4 }, 0 });

		//building->addSectorWindow(0, StorageDeck, 10, 2, 1);
		//building->addSectorWindow(1, StorageDeck, 13, 1, 1);

		//
		// Flight deck
		//
		building->addCorridor(FlightDeck, 6, 4);

		//
		// Join decks
		//
		building->addLadder(ReactorDeck, 4, { 3, true, true });
		building->addLadder(HospitalDeck + 1, 18, { 2, true, true });
		building->addStairwell(ReactorDeck, 19, 4, CORE_SIDE_LEFT);
		building->addLift(ReactorDeck, 17, { 1, { 0, 1, 3 } });
		building->addLift(HospitalDeck, 6, { 2, { 0, 1, 2, 3 } });

		// Add Windows now that we've placed objects on both Layers
		building->addSectorWindow(1, HospitalDeck, 4, 1, 1);
		building->addSectorWindow(0, MaintenanceDeck, 17, 1, 1);
		building->addSectorWindow(1, MaintenanceDeck, 0, 1, 1);
		building->addSectorWindow(1, MaintenanceDeck, 12, 1, 1);
*/
		building->finishBuild();

		// Add agents

		// Door test agents
		/*
		for (int i = 0; i < 4; ++i)
		{
			if (i == 8) continue;
			
			int xx = i & 1 ? 9 - i : i / 2;
			
			// Fore
			auto agentId = building->createAgent(format("PathAgentF {}", i + 1),
				morgueCorrIndex, 0, xx + 0.5f);
			auto agent = building->lookupAgent(agentId).entity;

			// Generate path
			auto graph = building->getGraph();
			auto vertex = graph->getVertexByIdentifier(vertexIdentifiers[i * 2 + 1]);
			auto path = core::pathing::findPath(agent, graph.get(), nullptr, vertex);
			agent->setPath(path, false);

			// Back
			//agent = new core::Agent(format("PathAgentB {}", i + 1));

			//vertex = graph->getVertexByIdentifier(vertexIdentifiers[i * 2]);
			//path = core::pathing::findPath(agent, graph.get(), nullptr, vertex);
			//agent->setPath(path, false);
		}
		*/

		//auto agent = new core::Agent(format("PathAgent-ButtonTest"));

		// Generate path
		//auto graph = building->getGraph();
		//auto vertex = graph->getVertexByIdentifier(vertexIdentifiers[19]);
		//auto path = core::pathing::findPath(agent, graph.get(), nullptr, vertex);
		//agent->setPath(path, false);
	}
	catch (core::BuildingException& e)
	{
		auto const& buildLog = building->getBuildLog();

		for (auto const& entry : buildLog)
		{
			auto const& [source, sourceId, level, msg] = entry;

			string output = format("{}: {}", source, msg);

			switch (level)
			{
			case core::LogLevel::Debug:
				gLogger->debug(output);
				break;

			case core::LogLevel::Info:
				gLogger->info(output);
				break;

			case core::LogLevel::Warning:
				gLogger->warn(output);
				break;

			case core::LogLevel::Error:
				gLogger->error(output);
				break;
			}
		}

		throw e;
	}

	return building;
}


void run()
{
	shared_ptr<core::Building> building;// createTestBuilding();
	std::shared_ptr<core::Agent> pathingAgent = make_shared<core::Agent>("Pather");

	// Render settings
	ImVec4 clearColour = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	//
	// Main loop
	//
#if defined(_WIN32)
	LARGE_INTEGER startingTime, endingTime, elapsedMicroseconds;
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&startingTime);
#elif defined(__linux__)
	auto previousTime = chrono::steady_clock::now();
#else
#error "Unsupported platform"
#endif

	bool done = false, showDemoWindow = false;
	while (!done)
	{
		// Get elapsed time
#if defined(_WIN32)
		QueryPerformanceCounter(&endingTime);
		elapsedMicroseconds.QuadPart = endingTime.QuadPart - startingTime.QuadPart;
		elapsedMicroseconds.QuadPart *= 1000000;
		elapsedMicroseconds.QuadPart /= frequency.QuadPart;
		startingTime = endingTime;
		auto const updateTimeMicros = elapsedMicroseconds.QuadPart;
#elif defined(__linux__)
		auto const currentTime = chrono::steady_clock::now();
		auto const updateTimeMicros = chrono::duration_cast<chrono::microseconds>(
			currentTime - previousTime).count();
		previousTime = currentTime;
#else
#error "Unsupported platform"
#endif

		// Events. Window-manager close requests are resolved after NewFrame so the
		// UI can open the same unsaved-changes confirmation used by File actions.
		bool const closeRequested = processEvents(gWindow);

		// Logic
		float updateTimeSecs = updateTimeMicros / 1'000'000.0f;

		if (building)
		{
			building->update(gUISettings.worldPaused ? 0.0f : updateTimeSecs);
			// The current UI observes entity state directly. Drain value events until
			// an event-driven UI consumer is introduced so the queue remains bounded.
			(void)building->consumeSimulationEvents();
		}

		// Set up rendering
		int drawableWidth, drawableHeight;
		SDL_GL_GetDrawableSize(gWindow, &drawableWidth, &drawableHeight);
		glViewport(0, 0, drawableWidth, drawableHeight);
		glClearColor(clearColour.x * clearColour.w, clearColour.y * clearColour.w, clearColour.z * clearColour.w, clearColour.w);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

		// Start the Dear ImGui frame
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();

		ImGui::NewFrame();

		if (closeRequested) done = requestApplicationClose(building);
		handleShortcuts(building);
		if (building) handleContinuousKeyboardInput(building, updateTimeMicros);

		if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_F11)))
		{
			showDemoWindow = !showDemoWindow;
		}

		if (showDemoWindow)
		{
			ImGui::SetNextWindowFocus();
			ImGui::ShowDemoWindow();
		}

		renderUI(building, pathingAgent);

		if (building)
		{
			auto mouseButtonStatus = getMouseButtonStatus();
			handleWorldInteraction(building, building->getGraph(), mouseButtonStatus);
		}

		// Rendering
		ImGui::Render();

		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		SDL_GL_SwapWindow(gWindow);
	}
}


void outputToDebugger(std::string const& msg)
{
#if defined(_DEBUG) && defined(_WIN32)
	char const* msgc = msg.c_str();

	size_t reqLength = ::MultiByteToWideChar(CP_UTF8, 0, msgc, (int)strlen(msgc), 0, 0);
	std::wstring ret(reqLength, L'\0');

	::MultiByteToWideChar(CP_UTF8, 0, msgc, (int)strlen(msgc), &ret[0], (int)ret.length());
	OutputDebugString(ret.c_str());
#else
	(void)msg;
#endif
}


void outputException(std::string const& msg)
{
	if (gLogger)
	{
		gLogger->critical(msg);
	}
	else
	{
		// Startup can fail before or during logging setup, so keep a fallback
		// channel for the diagnostic.
		fprintf(stderr, "Critical: %s\n", msg.c_str());
	}

	outputToDebugger(msg);
}


//
// Entrypoint
//
int main(int, char**)
{
	int exitCode{ 0 };

	try
	{
		initialise();
		setup();
		run();
	}
	catch (ExitApplicationException& e)
	{
		exitCode = e.getExitCode();
		if (exitCode != 0)
		{
			outputException(e.what());
		}
	}
	catch (exception& e)
	{
		exitCode = 1;
		outputException(e.what());
	}

	shutdown();
	
	return exitCode;
}
