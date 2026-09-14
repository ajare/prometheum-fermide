cmake_minimum_required(VERSION 3.24)

project(prometheum-fermide LANGUAGES CXX)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "prometheum-fermide supports Windows and Linux only")
endif()
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "Windows builds require MSVC and the bundled MSVC libraries")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "prometheum-fermide only supports 64-bit builds")
endif()

# The bundled Windows libraries and supported Linux builds provide these two
# configurations only. Single-configuration generators use Release by default.
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Available build configurations" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(WIN32)
    set(PF_BUILD_GUI_DEFAULT ON)
else()
    # The Linux GUI needs platform development packages (SDL2, GLEW, spdlog,
    # nativefiledialog-extended, and their window-system backends). Keep a
    # fresh Linux checkout buildable with only a compiler, CMake, and Git.
    set(PF_BUILD_GUI_DEFAULT OFF)
endif()
option(PF_BUILD_GUI "Build the SDL/OpenGL graphical application" ${PF_BUILD_GUI_DEFAULT})

set(PF_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(PF_VENDOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/include")
set(PF_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/x64/$<CONFIG>")

if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)

    set(PF_VENDOR_LIBRARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lib/x64")
    function(pf_add_vendor_library target debug_filename release_filename)
        add_library(${target} UNKNOWN IMPORTED GLOBAL)
        set_target_properties(${target} PROPERTIES
            IMPORTED_LOCATION_DEBUG "${PF_VENDOR_LIBRARY_DIR}/Debug/${debug_filename}"
            IMPORTED_LOCATION_RELEASE "${PF_VENDOR_LIBRARY_DIR}/Release/${release_filename}"
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
            MAP_IMPORTED_CONFIG_MINSIZEREL Release
            INTERFACE_INCLUDE_DIRECTORIES "${PF_VENDOR_INCLUDE_DIR}"
        )
    endfunction()

    pf_add_vendor_library(pf_yaml_cpp yaml-cppd.lib yaml-cpp.lib)
    if(PF_BUILD_GUI)
        pf_add_vendor_library(pf_glew glew32.lib glew32.lib)
        pf_add_vendor_library(pf_nfd nfd.lib nfd.lib)
        pf_add_vendor_library(pf_spdlog spdlogd.lib spdlog.lib)
        pf_add_vendor_library(pf_sdl2 SDL2d.lib SDL2.lib)
        pf_add_vendor_library(pf_sdl2main SDL2maind.lib SDL2main.lib)
    endif()
else()
    # Prefer a distro package, but keep a fresh checkout buildable when the
    # yaml-cpp development package is not installed.
    find_package(yaml-cpp CONFIG QUIET)
    if(NOT TARGET yaml-cpp::yaml-cpp AND NOT TARGET yaml-cpp)
        include(FetchContent)
        set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
        set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
        # CMake 4 requires an explicit policy floor when configuring
        # dependencies that retain compatibility with older CMake releases.
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
        FetchContent_Declare(yaml-cpp
            GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
            # Pinned newer than 0.8.0, which does not compile with GCC 15.
            GIT_TAG 8eb618e889c003457d73f7c54777603835677ff4
        )
        FetchContent_MakeAvailable(yaml-cpp)
    endif()

    if(TARGET yaml-cpp::yaml-cpp)
        set(PF_YAML_TARGET yaml-cpp::yaml-cpp)
    elseif(TARGET yaml-cpp)
        set(PF_YAML_TARGET yaml-cpp)
    else()
        message(FATAL_ERROR "The yaml-cpp package did not provide a supported CMake target")
    endif()

    if(PF_BUILD_GUI)
        find_package(OpenGL REQUIRED)
        find_package(GLEW REQUIRED)
        find_package(SDL2 REQUIRED)
        find_package(spdlog CONFIG REQUIRED)
        find_package(nfd CONFIG REQUIRED)

        if(TARGET nfd::nfd)
            set(PF_NFD_TARGET nfd::nfd)
        elseif(TARGET NFD::nfd)
            set(PF_NFD_TARGET NFD::nfd)
        elseif(TARGET nfd)
            set(PF_NFD_TARGET nfd)
        else()
            message(FATAL_ERROR "The nfd package did not provide nfd::nfd, NFD::nfd, or nfd")
        endif()
    endif()
endif()

file(GLOB PF_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
file(GLOB PF_CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/core/*.h")

add_library(prometheum-fermide-core STATIC ${PF_CORE_SOURCES} ${PF_CORE_HEADERS})
target_compile_features(prometheum-fermide-core PUBLIC cxx_std_20)
target_compile_definitions(prometheum-fermide-core PUBLIC YAML_CPP_STATIC_DEFINE)
target_include_directories(prometheum-fermide-core PUBLIC "${PF_INCLUDE_DIR}")
if(MSVC)
    target_compile_options(prometheum-fermide-core PRIVATE /W3 /permissive-)
else()
    target_compile_options(prometheum-fermide-core PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
    target_link_libraries(prometheum-fermide-core PUBLIC pf_yaml_cpp)
else()
    target_link_libraries(prometheum-fermide-core PUBLIC ${PF_YAML_TARGET})
endif()
set_target_properties(prometheum-fermide-core PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    OUTPUT_NAME "prometheum-fermide-core"
)

add_executable(prometheum-fermide-headless
    src/headless/InteractionApiCompileCheck.cpp
    src/headless/SerializationSmokeChecks.cpp
    src/headless/SmokeScenario.cpp
)
target_compile_features(prometheum-fermide-headless PRIVATE cxx_std_20)
target_compile_definitions(prometheum-fermide-headless PRIVATE _CONSOLE)
target_link_libraries(prometheum-fermide-headless PRIVATE prometheum-fermide-core)
if(MSVC)
    target_compile_options(prometheum-fermide-headless PRIVATE /W3 /permissive-)
    target_link_libraries(prometheum-fermide-headless PRIVATE Psapi)
else()
    target_compile_options(prometheum-fermide-headless PRIVATE -Wall -Wextra -Wpedantic)
endif()
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
        target_link_libraries(imgui PRIVATE
            prometheum-fermide-core pf_glew pf_nfd pf_spdlog pf_sdl2main pf_sdl2 opengl32
        )
        target_link_options(imgui PRIVATE "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>")
    else()
        target_compile_options(imgui PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(imgui PRIVATE
            prometheum-fermide-core GLEW::GLEW ${PF_NFD_TARGET}
            spdlog::spdlog SDL2::SDL2 OpenGL::GL
        )
    endif()
    set_target_properties(imgui PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    )

    add_custom_command(TARGET imgui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/prometheum-fermide.ini"
            "$<TARGET_FILE_DIR:imgui>/prometheum-fermide.ini"
        COMMENT "Copying ImGui configuration"
    )
    if(WIN32)
        add_custom_command(TARGET imgui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/vendor/bin/x64/$<CONFIG>"
                "$<TARGET_FILE_DIR:imgui>"
            COMMENT "Copying Windows runtime libraries"
        )
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME headless-smoke COMMAND prometheum-fermide-headless)
endif()
