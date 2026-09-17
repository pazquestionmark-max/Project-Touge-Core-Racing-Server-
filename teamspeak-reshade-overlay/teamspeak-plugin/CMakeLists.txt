# TeamSpeak 3 client plugin (Component B).
#
# The SDK headers are not redistributed here. scripts/fetch-deps fetches them into
# third_party/ts3client-pluginsdk; TSRO_TS3_SDK_DIR overrides the location.

set(TSRO_TS3_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/ts3client-pluginsdk"
    CACHE PATH "Path to a checkout of TeamSpeak/ts3client-pluginsdk")

if(NOT EXISTS "${TSRO_TS3_SDK_DIR}/include/ts3_functions.h")
  message(FATAL_ERROR
    "TeamSpeak Plugin SDK headers not found under '${TSRO_TS3_SDK_DIR}'.\n"
    "Run scripts/fetch-deps.ps1 (or fetch-deps.sh), or pass -DTSRO_TS3_SDK_DIR=<path>.")
endif()

add_library(tsro_ts3plugin SHARED
  src/plugin.cpp
  src/plugin_core.cpp
  src/ts_state.cpp
  src/ts_query_ts3.cpp
)

target_include_directories(tsro_ts3plugin
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${TSRO_TS3_SDK_DIR}/include
)

target_link_libraries(tsro_ts3plugin PRIVATE tsro_shared)

# TeamSpeak loads plugins by file name and distinguishes architectures by the _win64 suffix.
set_target_properties(tsro_ts3plugin PROPERTIES
  OUTPUT_NAME "tsro_overlay_win64"
  PREFIX ""
)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set_target_properties(tsro_ts3plugin PROPERTIES OUTPUT_NAME "tsro_overlay_win32")
endif()


install(TARGETS tsro_ts3plugin RUNTIME DESTINATION plugin LIBRARY DESTINATION plugin)
