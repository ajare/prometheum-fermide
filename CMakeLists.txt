cmake_minimum_required(VERSION 3.24)

project(phosphorus-fluoride LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "phosphorus-fluoride currently requires MSVC and the bundled Windows libraries")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "phosphorus-fluoride only supports 64-bit builds; configure with -A x64")
endif()

# The bundled libraries are provided for these two configurations only.
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Available build configurations" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_compile_definitions(UNICODE _UNICODE)

set(PF_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(PF_VENDOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/include")
set(PF_VENDOR_LIBRARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lib/x64")
set(PF_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/$<CONFIG>")

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
pf_add_vendor_library(pf_glew glew32.lib glew32.lib)
pf_add_vendor_library(pf_nfd nfd.lib nfd.lib)
pf_add_vendor_library(pf_spdlog spdlogd.lib spdlog.lib)
pf_add_vendor_library(pf_sdl2 SDL2d.lib SDL2.lib)
pf_add_vendor_library(pf_sdl2main SDL2maind.lib SDL2main.lib)

file(GLOB PF_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
file(GLOB PF_CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/core/*.h")

add_library(phosphorus-fluoride-core STATIC ${PF_CORE_SOURCES} ${PF_CORE_HEADERS})
target_compile_features(phosphorus-fluoride-core PUBLIC cxx_std_20)
target_compile_definitions(phosphorus-fluoride-core PUBLIC YAML_CPP_STATIC_DEFINE)
target_compile_options(phosphorus-fluoride-core PRIVATE /W3 /permissive-)
target_include_directories(phosphorus-fluoride-core
    PUBLIC
        "${PF_INCLUDE_DIR}"
        "${PF_VENDOR_INCLUDE_DIR}"
)
target_link_libraries(phosphorus-fluoride-core PUBLIC pf_yaml_cpp)
set_target_properties(phosphorus-fluoride-core PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
    OUTPUT_NAME "phosphorus-fluoride-core"
)

add_executable(phosphorus-fluoride-headless
    src/headless/InteractionApiCompileCheck.cpp
    src/headless/SerializationSmokeChecks.cpp
    src/headless/SmokeScenario.cpp
)
target_compile_options(phosphorus-fluoride-headless PRIVATE /W3 /permissive-)
target_compile_definitions(phosphorus-fluoride-headless PRIVATE _CONSOLE)
target_link_libraries(phosphorus-fluoride-headless PRIVATE phosphorus-fluoride-core Psapi)
set_target_properties(phosphorus-fluoride-headless PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
)

file(GLOB PF_IMGUI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/imgui/*.cpp")
add_executable(imgui
    src/Helpers.cpp
    src/Main.cpp
    src/Render.cpp
    src/UI.cpp
    ${PF_IMGUI_SOURCES}
)
target_compile_options(imgui PRIVATE /W3 /permissive-)
target_compile_definitions(imgui PRIVATE
    _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
)
target_include_directories(imgui PRIVATE "${PF_INCLUDE_DIR}/imgui")
target_link_libraries(imgui PRIVATE
    phosphorus-fluoride-core
    pf_glew
    pf_nfd
    pf_spdlog
    pf_sdl2main
    pf_sdl2
    opengl32
)
target_link_options(imgui PRIVATE "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>")
set_target_properties(imgui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
)

add_custom_command(TARGET imgui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/bin/x64/$<CONFIG>"
        "$<TARGET_FILE_DIR:imgui>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:imgui>"
    COMMENT "Copying runtime libraries and resources"
)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME headless-smoke COMMAND phosphorus-fluoride-headless)
endif()
