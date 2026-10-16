cmake_minimum_required(VERSION 3.20)
project(darkwing_duck_recomp VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Release builds link a static SDL3 built from source, so players don't need
# to install anything.
option(DWD_BUNDLED_SDL "Build SDL3 from source and link it statically" OFF)
option(DWD_PACKAGE "Build a distributable app (macOS .app bundle)" OFF)

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
if(NOT DWD_BUNDLED_SDL)
  find_package(SDL3 CONFIG QUIET)
endif()
if(NOT SDL3_FOUND)
  message(STATUS "SDL3 not found on the system, fetching it")
  include(FetchContent)
  FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.2.24)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3)
endif()

# Dear ImGui for the in-game menu.
include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.8)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

set(ROM_PATH "${CMAKE_SOURCE_DIR}/rom/darkwing-duck.nes" CACHE FILEPATH "Your Darkwing Duck ROM")
set(GAME_CONFIG "${CMAKE_SOURCE_DIR}/config/darkwing_duck.toml")
set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
set(NESRECOMP ${CMAKE_COMMAND} -E env "PYTHONPATH=${CMAKE_SOURCE_DIR}/tools" ${Python3_EXECUTABLE} -m nesrecomp)
file(GLOB NESRECOMP_PY "${CMAKE_SOURCE_DIR}/tools/nesrecomp/*.py")

if(NOT EXISTS "${ROM_PATH}")
  message(FATAL_ERROR "ROM not found at ${ROM_PATH}.\n"
          "Copy your own dump of Darkwing Duck there, or pass -DROM_PATH=/path/to/rom.nes")
endif()

# ---- Interpreter (generated from the same semantics as the recompiler) -----
add_custom_command(
  OUTPUT "${GEN_DIR}/interp_gen.c"
  COMMAND ${NESRECOMP} interpreter --out "${GEN_DIR}/interp_gen.c"
  DEPENDS ${NESRECOMP_PY}
  COMMENT "Generating 6502 interpreter")

add_library(nesrt STATIC
  runtime/src/cart.c
  runtime/src/cpu.c
  runtime/src/ppu.c
  runtime/src/apu.c
  runtime/src/state.c
  "${GEN_DIR}/interp_gen.c")
target_include_directories(nesrt PUBLIC runtime/include)

# ---- Recompile the game at configure time and on input changes -------------
set(RECOMP_INPUTS "${ROM_PATH}" "${GAME_CONFIG}" "${CMAKE_SOURCE_DIR}/config/darkwing_duck.entries" ${NESRECOMP_PY})
execute_process(
  COMMAND ${NESRECOMP} recompile --config "${GAME_CONFIG}" --rom "${ROM_PATH}" --out "${GEN_DIR}/game"
  RESULT_VARIABLE RECOMP_RESULT)
if(NOT RECOMP_RESULT EQUAL 0)
  message(FATAL_ERROR "Recompilation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_INPUTS})
include("${GEN_DIR}/game/sources.cmake")

add_library(game_recomp STATIC ${NESRECOMP_GENERATED})
target_link_libraries(game_recomp PUBLIC nesrt)
# Window functions are huge single functions; -O1 keeps compile times sane.
target_compile_options(game_recomp PRIVATE -O1 -Wno-unused-label)

# Built-in bezel artwork, compiled into the executable.
set(NIGHT_CITY_PNG "${CMAKE_SOURCE_DIR}/assets/bezels/night_city.png")
add_custom_command(
  OUTPUT "${GEN_DIR}/bezel_night_city.c"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/cmake/embed.py" "${NIGHT_CITY_PNG}" night_city_png
          "${GEN_DIR}/bezel_night_city.c"
  DEPENDS "${NIGHT_CITY_PNG}" "${CMAKE_SOURCE_DIR}/cmake/embed.py")

add_executable(darkwing-duck src/main.c src/bezel.c src/rom_select.c src/ui.cpp "${GEN_DIR}/bezel_night_city.c")
target_include_directories(darkwing-duck PRIVATE third_party)
target_compile_definitions(darkwing-duck PRIVATE DWD_VERSION="${PROJECT_VERSION}")
target_link_libraries(darkwing-duck PRIVATE game_recomp nesrt imgui SDL3::SDL3)

# ---- Packaging ----------------------------------------------------------------
if(APPLE AND DWD_PACKAGE)
  set(ICNS "${CMAKE_SOURCE_DIR}/packaging/macos/icon.icns")
  target_sources(darkwing-duck PRIVATE "${ICNS}")
  set_source_files_properties("${ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(darkwing-duck PROPERTIES
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "Darkwing Duck Recomp"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
elseif(APPLE)
  # plain executable for development builds
elseif(WIN32)
  target_sources(darkwing-duck PRIVATE "${CMAKE_SOURCE_DIR}/packaging/windows/icon.rc")
  set_target_properties(darkwing-duck PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MINGW)
    target_link_options(darkwing-duck PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
else()
  target_link_options(darkwing-duck PRIVATE -static-libgcc -static-libstdc++)
endif()
install(TARGETS darkwing-duck BUNDLE DESTINATION . RUNTIME DESTINATION bin)
