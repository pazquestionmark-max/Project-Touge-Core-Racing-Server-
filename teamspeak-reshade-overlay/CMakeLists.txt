cmake_minimum_required(VERSION 3.20)
project(teamspeak-reshade-overlay VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TSRO_BUILD_TESTS "Build the unit and integration test suite" ON)
option(TSRO_BUILD_PLUGIN "Build the TeamSpeak 3 plugin (Windows only)" OFF)
option(TSRO_BUILD_ADDON "Build the ReShade add-on (Windows only)" OFF)
option(TSRO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static CRT for every target, set once here rather than per target. The plugin and the add-on
# need it -- a DLL that depends on a redistributable the user has not installed simply fails to
# load, with no diagnostic they can act on -- and a static library built against the dynamic CRT
# cannot be linked into them (LNK2038). Setting it in one place is what stops those drifting.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# These two live in the game process and the voice client; both are built for size and speed
# with no runtime the user must install separately.
if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  if(TSRO_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(TSRO_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

add_subdirectory(shared)
add_subdirectory(tools/config-tool)

if(TSRO_BUILD_PLUGIN)
  add_subdirectory(teamspeak-plugin)
endif()

if(TSRO_BUILD_ADDON)
  add_subdirectory(reshade-integration)
endif()

if(TSRO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
