cmake_minimum_required(VERSION 3.20)
project(darkwing_duck_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
  message(STATUS "SDL3 not found on the system, fetching it")
  include(FetchContent)
  FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.2.24)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3)
endif()

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

add_executable(darkwing-duck src/main.c)
target_link_libraries(darkwing-duck PRIVATE game_recomp nesrt SDL3::SDL3)
