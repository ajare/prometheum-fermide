cmake_minimum_required(VERSION 3.24)

project(prometheum-fermide LANGUAGES C CXX)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "prometheum-fermide supports Windows and Linux only")
endif()
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "Windows builds require MSVC")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "prometheum-fermide only supports 64-bit builds")
endif()

# Supported builds provide these two configurations only. Single-configuration
# generators use Release by default.
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Available build configurations" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PF_BUILD_GUI "Build the SDL/OpenGL graphical application" ON)

# High analysis settings (tickets #56, #79). The refactor of Building into
# Building plus SimulationCoordinator was driven by a translation unit that
# choked the compiler under elevated analysis; this option keeps that check
# repeatable. It raises the diagnostic level for the project's own targets
# only - vendored third-party sources keep their own warning configuration -
# and tools/high_analysis_build.sh pairs it with a per-translation-unit
# time/memory report so a TU that starts to choke is noticed, not tolerated.
option(PF_HIGH_ANALYSIS "Compile project targets with elevated static-analysis diagnostics" OFF)

set(PF_HIGH_ANALYSIS_WARNINGS "")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(PF_HIGH_ANALYSIS_WARNINGS
        -Wcast-align
        -Wcast-qual
        -Wdouble-promotion
        -Wextra-semi
        -Wfloat-equal
        -Wimplicit-fallthrough
        -Wmissing-declarations
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wredundant-decls
        -Wshadow
        -Wundef
        -Wunused-but-set-variable
    )
elseif(MSVC)
    set(PF_HIGH_ANALYSIS_WARNINGS /W4 /permissive-)
endif()

# Applied to project targets only; never to FetchContent dependencies.
function(pf_apply_high_analysis target)
    if(PF_HIGH_ANALYSIS AND PF_HIGH_ANALYSIS_WARNINGS)
        target_compile_options(${target} PRIVATE ${PF_HIGH_ANALYSIS_WARNINGS})
    endif()
endfunction()

set(PF_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(PF_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/x64/$<CONFIG>")

include(FetchContent)
# CMake 4 requires an explicit policy floor when configuring dependencies
# that retain compatibility with older CMake releases.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)
    # FetchContent dependencies must use the same DLL runtime as the project.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Third-party libraries are pinned and built from source on every supported
# platform so Windows no longer depends on the checked-in vendor libraries.
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    # Pinned newer than 0.8.0, which does not compile with GCC 15.
    GIT_TAG 8eb618e889c003457d73f7c54777603835677ff4
)
FetchContent_MakeAvailable(yaml-cpp)

# Agent behaviour modules use the exact runtime versions selected by ADR 0008.
# Lua is built as a private static C library and sol2 is header-only. Neither
# dependency inherits the warning policy used for project-owned targets.
FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.4.9.tar.gz
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(lua)
add_library(pf-lua STATIC
    "${lua_SOURCE_DIR}/src/lapi.c"
    "${lua_SOURCE_DIR}/src/lauxlib.c"
    "${lua_SOURCE_DIR}/src/lbaselib.c"
    "${lua_SOURCE_DIR}/src/lcode.c"
    "${lua_SOURCE_DIR}/src/lcorolib.c"
    "${lua_SOURCE_DIR}/src/lctype.c"
    "${lua_SOURCE_DIR}/src/ldblib.c"
    "${lua_SOURCE_DIR}/src/ldebug.c"
    "${lua_SOURCE_DIR}/src/ldo.c"
    "${lua_SOURCE_DIR}/src/ldump.c"
    "${lua_SOURCE_DIR}/src/lfunc.c"
    "${lua_SOURCE_DIR}/src/lgc.c"
    "${lua_SOURCE_DIR}/src/linit.c"
    "${lua_SOURCE_DIR}/src/liolib.c"
    "${lua_SOURCE_DIR}/src/llex.c"
    "${lua_SOURCE_DIR}/src/lmathlib.c"
    "${lua_SOURCE_DIR}/src/lmem.c"
    "${lua_SOURCE_DIR}/src/loadlib.c"
    "${lua_SOURCE_DIR}/src/lobject.c"
    "${lua_SOURCE_DIR}/src/lopcodes.c"
    "${lua_SOURCE_DIR}/src/loslib.c"
    "${lua_SOURCE_DIR}/src/lparser.c"
    "${lua_SOURCE_DIR}/src/lstate.c"
    "${lua_SOURCE_DIR}/src/lstring.c"
    "${lua_SOURCE_DIR}/src/lstrlib.c"
    "${lua_SOURCE_DIR}/src/ltable.c"
    "${lua_SOURCE_DIR}/src/ltablib.c"
    "${lua_SOURCE_DIR}/src/ltm.c"
    "${lua_SOURCE_DIR}/src/lundump.c"
    "${lua_SOURCE_DIR}/src/lutf8lib.c"
    "${lua_SOURCE_DIR}/src/lvm.c"
    "${lua_SOURCE_DIR}/src/lzio.c"
)
target_include_directories(pf-lua SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")
target_compile_definitions(pf-lua PRIVATE
    $<$<PLATFORM_ID:Linux>:LUA_USE_POSIX>
    $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
set_target_properties(pf-lua PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    OUTPUT_NAME "lua-5.4.9"
)

set(SOL2_BUILD_LUA OFF CACHE BOOL "" FORCE)
set(SOL2_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(SOL2_TESTS OFF CACHE BOOL "" FORCE)
set(SOL2_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    # Exact commit tagged v3.5.0.
    GIT_TAG e24392e9718f0616cfbba86005622a419d2f0c5d
)
FetchContent_MakeAvailable(sol2)

if(TARGET yaml-cpp::yaml-cpp)
    set(PF_YAML_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(PF_YAML_TARGET yaml-cpp)
else()
    message(FATAL_ERROR "The yaml-cpp dependency did not provide a supported CMake target")
endif()

if(PF_BUILD_GUI)
    # OpenGL is provided by the host; portable dependencies are built from
    # their pinned source revisions.
    find_package(OpenGL REQUIRED)

    set(glew-cmake_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(glew-cmake_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ONLY_LIBS ON CACHE BOOL "" FORCE)
    set(USE_GLU OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glew
        GIT_REPOSITORY https://github.com/Perlmint/glew-cmake.git
        GIT_TAG f456deace7b408655109aaeff71421ef2d3858c6
    )

    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_SDL2MAIN OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG 5d249570393f7a37e037abf22cd6012a4cc56a71
    )

    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG 6fa36017cfd5731d617e1a934f0e5ea9c4445b13
    )

    # The portal backend avoids a GTK development dependency on Linux. NFD
    # selects its native Windows backend automatically on Windows.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(NFD_PORTAL ON CACHE BOOL "" FORCE)
    endif()
    set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_BUILD_SDL2_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nfd
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG 86d5f2005fe1c00747348a12070fec493ea2407e
    )

    FetchContent_MakeAvailable(glew SDL2 spdlog nfd)
    # SDL 2.32's C23 bsearch wrapper triggers this GCC 15 diagnostic.
    target_compile_options(SDL2-static PRIVATE
        $<$<COMPILE_LANG_AND_ID:C,GNU>:-Wno-discarded-qualifiers>
    )
endif()

file(GLOB PF_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
file(GLOB PF_CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/core/*.h")

add_library(prometheum-fermide-core STATIC
    ${PF_CORE_SOURCES}
    ${PF_CORE_HEADERS}
    src/RecentFiles.cpp
    include/RecentFiles.h
    src/DocumentHistory.cpp
    include/DocumentHistory.h
)
target_compile_features(prometheum-fermide-core PUBLIC cxx_std_20)
target_compile_definitions(prometheum-fermide-core PUBLIC YAML_CPP_STATIC_DEFINE)
target_include_directories(prometheum-fermide-core PUBLIC "${PF_INCLUDE_DIR}")
if(MSVC)
    target_compile_options(prometheum-fermide-core PRIVATE /W3 /permissive-)
else()
    target_compile_options(prometheum-fermide-core PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(prometheum-fermide-core
    PUBLIC ${PF_YAML_TARGET}
    PRIVATE pf-lua sol2::sol2
)
pf_apply_high_analysis(prometheum-fermide-core)
set_target_properties(prometheum-fermide-core PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    OUTPUT_NAME "prometheum-fermide-core"
)

add_executable(prometheum-fermide-headless
    # The real renderer, linked headless so #49 can exercise renderSector's
    # actual draw-call order. ImGui is compiled CPU-side only; the GUI-owned
    # globals are stubbed in RenderGuiStubs.cpp. The Door panel is compiled in
    # too (#99) so its disabled-scope balance is checked against the real panel.
    src/Render.cpp
    src/DocumentEdit.cpp
    src/DoorPanel.cpp
    src/AgentGroupsPanel.cpp
    src/AgentGroupAssignmentPanel.cpp
    src/AgentTagAssignmentPanel.cpp
    src/AgentBehaviourAssignmentPanel.cpp
    src/TagsPanel.cpp
    src/BehavioursPanel.cpp
    src/AgentClipboard.cpp
    src/imgui/imgui.cpp
    src/imgui/imgui_draw.cpp
    src/imgui/imgui_tables.cpp
    src/imgui/imgui_widgets.cpp
    src/headless/BackgroundPaintSmokeChecks.cpp
    src/headless/BackgroundPlacementSmokeChecks.cpp
    src/headless/BackgroundCascadeDeleteSmokeChecks.cpp
    src/headless/BackgroundSelectionPanelSmokeChecks.cpp
    src/headless/AgentGroupSmokeChecks.cpp
    src/headless/AgentGroupAssignmentSmokeChecks.cpp
    src/headless/AgentGroupCountSmokeChecks.cpp
    src/headless/AgentGroupDeleteSmokeChecks.cpp
    src/headless/AgentGroupIdAllocationSmokeChecks.cpp
    src/headless/AgentGroupClipboardSmokeChecks.cpp
    src/headless/AgentGroupTopologySmokeChecks.cpp
    src/headless/AgentActivationSmokeChecks.cpp
    src/headless/DoorPanelScopeSmokeChecks.cpp
    src/headless/DoorTwoSidedButtonSmokeChecks.cpp
    src/headless/DocumentHistorySmokeChecks.cpp
    src/headless/AgentTagRegistrySmokeChecks.cpp
    src/headless/AgentTagRegistryChangeSmokeChecks.cpp
    src/headless/AgentTagDocumentSaveSmokeChecks.cpp
    src/headless/AgentTagReloadSmokeChecks.cpp
    src/headless/AgentTagAssignmentSmokeChecks.cpp
    src/headless/AgentTagDeleteSmokeChecks.cpp
    src/headless/AgentTagCoordinationSmokeChecks.cpp
    src/headless/AgentTagReconciliationSmokeChecks.cpp
    src/headless/AgentTagClipboardSmokeChecks.cpp
    src/headless/AgentColourSmokeChecks.cpp
    src/headless/AgentWalkSpeedSmokeChecks.cpp
    src/headless/AgentHeightSmokeChecks.cpp
    src/headless/AgentBehaviourRegistrySmokeChecks.cpp
    src/headless/AgentBehaviourAssignmentSmokeChecks.cpp
    src/headless/AgentBehaviourSchemaReconciliationSmokeChecks.cpp
    src/headless/AgentBehaviourRuntimeSmokeChecks.cpp
    src/headless/BackgroundSectorSmokeChecks.cpp
    src/headless/EditorLayerSmokeChecks.cpp
    src/headless/DoorOpenApartRenderSmokeChecks.cpp
    src/headless/DoorOpenLeftRenderSmokeChecks.cpp
    src/headless/DoorOpenRightRenderSmokeChecks.cpp
    src/headless/FacadeSmokeChecks.cpp
    src/headless/FacadeEditorSmokeChecks.cpp
    src/headless/FacadeRenderSmokeChecks.cpp
    src/headless/FacadeDrawOrderSmokeChecks.cpp
    src/headless/GraphicsStartupSmokeChecks.cpp
    src/headless/InteractionApiCompileCheck.cpp
    src/headless/IsolatedSectorPathingSmokeChecks.cpp
    src/headless/MarkerIdentitySmokeChecks.cpp
    src/headless/MovementCommandSmokeChecks.cpp
    src/headless/OnboardAgentDeletionSmokeChecks.cpp
    src/headless/PaletteTraySmokeChecks.cpp
    src/headless/RenderGuiStubs.cpp
    src/headless/RenderOrderSmokeChecks.cpp
    src/headless/SerializationSmokeChecks.cpp
    src/headless/ShuttleDoorQuerySmokeChecks.cpp
    src/headless/SmokeScenario.cpp
    src/headless/ThresholdRefusalSmokeChecks.cpp
    src/headless/ThresholdLayerOverlapSmokeChecks.cpp
    src/headless/WallRenderSmokeChecks.cpp
    src/headless/ViewportCullingSmokeChecks.cpp
    src/headless/WindowLayerSmokeChecks.cpp
    src/headless/WindowIntoBackgroundSmokeChecks.cpp
    src/headless/WindowMultiBackgroundSmokeChecks.cpp
    src/headless/ZeroSizeLocationSmokeChecks.cpp
)
target_compile_features(prometheum-fermide-headless PRIVATE cxx_std_20)
target_include_directories(prometheum-fermide-headless PRIVATE "${PF_INCLUDE_DIR}/imgui")
target_compile_definitions(prometheum-fermide-headless PRIVATE
    _CONSOLE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING>
)
target_link_libraries(prometheum-fermide-headless PRIVATE prometheum-fermide-core)
if(MSVC)
    target_compile_options(prometheum-fermide-headless PRIVATE /W3 /permissive-)
    target_link_libraries(prometheum-fermide-headless PRIVATE Psapi)
else()
    target_compile_options(prometheum-fermide-headless PRIVATE -Wall -Wextra -Wpedantic)
endif()
pf_apply_high_analysis(prometheum-fermide-headless)
set_target_properties(prometheum-fermide-headless PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
)

if(PF_BUILD_GUI)
    file(GLOB PF_IMGUI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui/*.cpp")
    add_executable(imgui
        src/Helpers.cpp
        src/Main.cpp
        src/Render.cpp
        src/UI.cpp
        src/DocumentEdit.cpp
        src/DoorPanel.cpp
        src/AgentGroupsPanel.cpp
        src/AgentGroupAssignmentPanel.cpp
        src/AgentTagAssignmentPanel.cpp
        src/AgentBehaviourAssignmentPanel.cpp
        src/TagsPanel.cpp
        src/BehavioursPanel.cpp
        src/AgentClipboard.cpp
        ${PF_IMGUI_SOURCES}
    )
    target_compile_features(imgui PRIVATE cxx_std_20)
    target_compile_definitions(imgui PRIVATE
        _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
        $<$<CONFIG:Debug>:_DEBUG>
    )
    target_include_directories(imgui PRIVATE "${PF_INCLUDE_DIR}/imgui")
    if(MSVC)
        target_compile_options(imgui PRIVATE /W3 /permissive-)
        target_link_options(imgui PRIVATE "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>")
    else()
        target_compile_options(imgui PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_link_libraries(imgui PRIVATE
        prometheum-fermide-core libglew_static nfd::nfd
        spdlog::spdlog SDL2::SDL2-static OpenGL::GL
    )
    if(WIN32)
        target_link_libraries(imgui PRIVATE SDL2::SDL2main)
    endif()
    pf_apply_high_analysis(imgui)
    set_target_properties(imgui PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    )

    add_custom_command(TARGET imgui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/prometheum-fermide.ini"
            "$<TARGET_FILE_DIR:imgui>/prometheum-fermide.ini"
        COMMENT "Copying ImGui configuration"
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME headless-smoke COMMAND prometheum-fermide-headless)

    if(PF_BUILD_GUI)
        # Ticket #59: the GUI must exit with a controlled non-zero code when
        # graphics initialisation fails, never via SIGABRT. Run from the GUI
        # output directory so the app's relative log/resource paths resolve
        # inside the build tree.
        add_test(NAME graphics-startup-failure
            COMMAND prometheum-fermide-headless --graphics-startup-smoke)
        set_tests_properties(graphics-startup-failure PROPERTIES
            ENVIRONMENT "PF_GUI_EXECUTABLE=$<TARGET_FILE:imgui>"
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:imgui>")
    endif()
endif()
