#define NOMINMAX

#include <Windows.h>

#include <glew/glew.h>
#include <nfd/nfd.h>

#pragma warning(push)
#pragma warning(disable: 4307)
#include <spdlog/spdlog.h>
#pragma warning(pop)
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/details/log_msg_buffer.h>

#include <SDL/SDL.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL/SDL_opengles2.h>
#else
#include <SDL/SDL_opengl.h>
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

#include "Main.h"
#include "Render.h"
#include "Helpers.h"
#include "UI.h"
#include "UISettings.h"
#include "Exceptions.h"


spdlog::logger* gLogger{ nullptr };
SDL_Window* gWindow{ nullptr };
SDL_GLContext gContext;
UISettings gUISettings;

GLuint gCellsTexture{ 0 };
int gCellsTextureWidth{ 0 };
int gCellsTextureHeight{ 0 };

using namespace std;

vector<LogMessage> gLogMessages;


SDL_Window* createWindow()
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
	{
		printf("Error: %s\n", SDL_GetError());
		return nullptr;
	}

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

	gLogger->info("Window created");

	return window;
}

SDL_GLContext createContext(SDL_Window* window)
{
	SDL_GLContext gl_context = SDL_GL_CreateContext(window);
	SDL_GL_MakeCurrent(window, gl_context);
	SDL_GL_SetSwapInterval(1); // Enable vsync

	gLogger->info("OpenGL context created");

	return gl_context;
}

void setupImGui(SDL_Window* window, SDL_GLContext context)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
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

	const char* glsl_version = "#version 130";
	ImGui_ImplOpenGL3_Init(glsl_version);

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
	string levelStr;
	switch (msg.level)
	{
	case spdlog::level::level_enum::debug:
		levelStr = "DEBUG";
		break;

	case spdlog::level::level_enum::info:
		levelStr = "INFO";
		break;

	case spdlog::level::level_enum::warn:
		levelStr = "WARN";
		break;

	case spdlog::level::level_enum::err:
		levelStr = "ERROR";
		break;

	case spdlog::level::level_enum::critical:
		levelStr = "CRIT";
		break;

	case spdlog::level::level_enum::trace:
		levelStr = "TRACE";
		break;

	default:
		levelStr = "?????";
		break;
	}

	gLogMessages.push_back({ levelStr, msg.payload });
});

void setupLogging()
{
#ifdef _DEBUG
	auto fileSink = make_shared<spdlog::sinks::basic_file_sink_mt>("../../../logs/pf-debug.log", true);
#else
	auto fileSink = make_shared<spdlog::sinks::basic_file_sink_mt>("../../../logs/pf-release.log", true);
#endif

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
		throw exception("GLEW initialisation failed");
	}

	//
	// Set up ImGui
	//
	setupImGui(gWindow, gContext);
}

void setup()
{
	// Set up NFD (file dialogs)
	NFD_Init();

	// ImGui extra twiddling
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	io.Fonts->AddFontDefault();

	float baseFontSize = 13.0f; // 13.0f is the size of the default font. Change to the font size you use.
	float iconFontSize = baseFontSize * 2.0f / 3.0f; // FontAwesome fonts need to have their sizes reduced by 2.0f/3.0f in order to align correctly

	static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
	ImFontConfig icons_config;
	icons_config.MergeMode = true;
	icons_config.PixelSnapH = true;
	icons_config.GlyphMinAdvanceX = iconFontSize;
	io.Fonts->AddFontFromFileTTF(FONT_ICON_FILE_NAME_FAS, iconFontSize, &icons_config, icons_ranges);
	io.Fonts->Build();
}

void shutdown()
{
	gLogger->info("Shutting down");

	delete gLogger;
	gLogger = nullptr;

	// ImGui
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();

	// Platform
	SDL_GL_DeleteContext(gContext);
	SDL_DestroyWindow(gWindow);
	SDL_Quit();

	NFD_Quit();
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
		auto reactorCorr1 = building->addCorridor(ReactorDeck, 1, 8);
		
		auto pumpRoomIndex = building->addRoom("Pump Room", CORE_LAYER_BACK, ReactorDeck, 0, 4, 1);
		building->addSectorLightSwitch(pumpRoomIndex, 0);

		building->addRoom("Fuel Cells", CORE_LAYER_BACK, ReactorDeck, 5, 3, 1);

		building->addSectorDoor(ReactorDeck, 6);
		
		//building->addSectorDoor(ReactorDeck, 2, core::Building::RemoteControlledDoor1Options);
		building->addSectorDoor(ReactorDeck, 2);

		building->addSectorWindow(CORE_LAYER_FORE, ReactorDeck, 7, 1, 1);
		
		// Second corridor
		auto corr2Index = building->addCorridor(ReactorDeck, 9, 6);

		// Reactor core
		auto reactorCoreIndex = building->addRoom("Reactor Core", CORE_LAYER_BACK, ReactorDeck, 9, 4, 3);
		
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

		building->addSectorWindow(CORE_LAYER_FORE, ReactorDeck, 1, 1, 1);
		
		// Decontamination
		building->addRoom("Decontamination", CORE_LAYER_BACK, ReactorDeck, 14, 3, 1);
		building->addSectorDoor(ReactorDeck, 14);

		building->addSectorWindow(CORE_LAYER_FORE, ReactorDeck, 11, 2, 1);

		// Connect corridors
		building->addSectorBulkheadDoor(CORE_LAYER_FORE, ReactorDeck, 9, CORE_SIDE_LEFT);
		
		// Third corridor
		building->addCorridor(ReactorDeck, 16, 14);

		//building->addSectorDoor(ReactorDeck, 16);

		// Fourth corridor
		building->addCorridor(ReactorDeck, 34, 9);

		//building->addSectorDoor(ReactorDeck, 38);

		building->addShuttle(ReactorDeck, 24, 17, { 2, 3, { 0, 10 }, 0 });


		//
		// Hospital
		//
		building->addCorridor(HospitalDeck, 1, 4);
		building->addCorridor(HospitalDeck, 6, 10);
		auto corrIndex = building->addCorridor(HospitalDeck, 17, 4, 2);

		building->addSectorWalkway(corrIndex, 1, 1);
		building->addSectorWalkway(corrIndex, 1, 2);
		building->addSectorWalkway(corrIndex, 1, 3);

		auto icuIndex = building->addRoom("ICU", CORE_LAYER_BACK, HospitalDeck, 13, 3, 1, CORE_DOOR_HEIGHT + 0.1f);
		building->addSectorDoor(HospitalDeck, 14);

		building->removeLocationWall(icuIndex, 0, CORE_SIDE_LEFT);

		auto acIndex = building->addRoom("Autoclaves", CORE_LAYER_BACK, HospitalDeck, 0, 3, 4, CORE_DOOR_HEIGHT + 0.1f);

		building->addSectorWalkway(acIndex, 1, 0);
		building->addSectorWalkway(acIndex, 1, 1);
		building->addSectorWalkway(acIndex, 2, 0);
		building->addSectorWalkway(acIndex, 2, 1);

		building->addSectorPlatformLift(acIndex, 0, 0, { 1, { 0, 1, 2 } });

		building->addSectorDoor(HospitalDeck, 2);
		
		auto morgueCorrIndex = building->addCorridor(HospitalDeck, 22, 9);
		auto morgueIndex = building->addRoom("Morgue", CORE_LAYER_BACK, HospitalDeck, 22, 9, 1);
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

		//building->addSectorWindow(CORE_LAYER_FORE, StorageDeck, 10, 2, 1);
		//building->addSectorWindow(CORE_LAYER_BACK, StorageDeck, 13, 1, 1);

		//
		// Flight deck
		//
		building->addCorridor(FlightDeck, 6, 4);

		//
		// Join decks
		//
		building->addLadder(ReactorDeck, 4, { 3, true, true });
		building->addLadder(HospitalDeck + 1, 18, { 2, true, true });
		building->addStaircase(ReactorDeck, 19, 4, CORE_SIDE_LEFT);
		building->addLift(ReactorDeck, 17, { 1, { 0, 1, 3 } });
		building->addLift(HospitalDeck, 6, { 2, { 0, 1, 2, 3 } });

		// Add Windows now that we've placed objects on both Layers
		building->addSectorWindow(CORE_LAYER_BACK, HospitalDeck, 4, 1, 1);
		building->addSectorWindow(CORE_LAYER_FORE, MaintenanceDeck, 17, 1, 1);
		building->addSectorWindow(CORE_LAYER_BACK, MaintenanceDeck, 0, 1, 1);
		building->addSectorWindow(CORE_LAYER_BACK, MaintenanceDeck, 12, 1, 1);

		building->finishBuild();

		// Add agents

		// Door test agents
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
	// Load cell images
	if (!LoadTextureFromFile("..\\..\\..\\resources\\cells.png", &gCellsTexture, &gCellsTextureWidth, &gCellsTextureHeight))
	{
		throw ExitApplicationException(1, "Could not load image for cells.");
	}

	shared_ptr<core::Building> building = createTestBuilding();
	std::shared_ptr<core::Agent> pathingAgent = make_shared<core::Agent>("Pather");

	// Render settings
	ImVec4 clearColour = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
	ImGuiIO& io = ImGui::GetIO();

	//
	// Main loop
	//
	LARGE_INTEGER StartingTime, EndingTime, ElapsedMicroseconds;
	LARGE_INTEGER Frequency;

	QueryPerformanceFrequency(&Frequency);
	QueryPerformanceCounter(&StartingTime);

	bool done = false, showDemoWindow = false;
	while (!done)
	{
		// Get elapsed time
		QueryPerformanceCounter(&EndingTime);
		ElapsedMicroseconds.QuadPart = EndingTime.QuadPart - StartingTime.QuadPart;
		ElapsedMicroseconds.QuadPart *= 1000000;
		ElapsedMicroseconds.QuadPart /= Frequency.QuadPart;

		StartingTime = EndingTime;

		auto updateTimeMicros = ElapsedMicroseconds.QuadPart;

		// Events
		done = processEvents(gWindow);

		// Logic
		float updateTimeSecs = updateTimeMicros / 1'000'000.0f;

		building->update(gUISettings.worldPaused ? 0.0f : updateTimeSecs);
		// The current UI observes entity state directly. Drain value events until
		// an event-driven UI consumer is introduced so the queue remains bounded.
		(void)building->consumeSimulationEvents();

		// Set up rendering
		glViewport(0, 0, APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT);
		glClearColor(clearColour.x * clearColour.w, clearColour.y * clearColour.w, clearColour.z * clearColour.w, clearColour.w);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

		// Start the Dear ImGui frame
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();

		ImGui::NewFrame();

		auto mouseButtonStatus = getMouseButtonStatus();

		handleShortcuts(building);
		handleWorldInteraction(building, building->getGraph(), mouseButtonStatus);
		handleContinuousKeyboardInput(building, updateTimeMicros);

		if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_F11)))
		{
			showDemoWindow = !showDemoWindow;
		}

		if (showDemoWindow)
		{
			ImGui::SetNextWindowFocus();
			ImGui::ShowDemoWindow();
		}

		renderUI(building, building->getGraph(), pathingAgent);

		// Rendering
		renderBuilding(building);
		renderGraph(building->getGraph(), building);
		
		ImGui::Render();

		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		SDL_GL_SwapWindow(gWindow);
	}
}


void outputToDebugger(std::string const& msg)
{
#ifdef _DEBUG
	char const* msgc = msg.c_str();

	size_t reqLength = ::MultiByteToWideChar(CP_UTF8, 0, msgc, (int)strlen(msgc), 0, 0);
	std::wstring ret(reqLength, L'\0');

	::MultiByteToWideChar(CP_UTF8, 0, msgc, (int)strlen(msgc), &ret[0], (int)ret.length());
	OutputDebugString(ret.c_str());
#endif
}


void outputException(std::string const& msg)
{
	gLogger->critical(msg);
	outputToDebugger(msg);
}


//
// Entrypoint
//
int main(int, char**)
{
	int exitCode{ 0 };

	initialise();

	try
	{
		setup();
		run();
	}
	catch (ExitApplicationException& e)
	{
		exitCode = e.getExitCode();
	}
	catch (exception& e)
	{
		exitCode = 1;
		outputException(e.what());
	}

	shutdown();
	
	return exitCode;
}
