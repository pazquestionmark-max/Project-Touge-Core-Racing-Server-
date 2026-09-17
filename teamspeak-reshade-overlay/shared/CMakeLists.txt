# The platform-independent core. No dependency on TeamSpeak, ReShade, ImGui or Win32, which is
# what makes the state machine, protocol, layout and configuration testable in CI on any host.
add_library(tsro_shared STATIC
  src/json.cpp
  src/model.cpp
  src/protocol.cpp
  src/framing.cpp
  src/state_store.cpp
  src/config.cpp
  src/config_enums.cpp
  src/config_defaults.cpp
  src/layout.cpp
  src/notifications.cpp
  src/log.cpp
  src/ipc_server.cpp
  src/overlay_client.cpp
  src/profile_store.cpp
  src/sqlite_read.cpp
  src/ts_contacts.cpp
  src/transport_win32.cpp
  src/transport_posix.cpp
)

target_include_directories(tsro_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(tsro_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(tsro_shared PUBLIC Threads::Threads)

if(WIN32)
  # advapi32 for the pipe security descriptor (ConvertStringSecurityDescriptorToSecurityDescriptor).
  target_link_libraries(tsro_shared PUBLIC advapi32)
  target_compile_definitions(tsro_shared PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# The commit the binary was built from, shown in the settings window.
#
# Three separate bug reports in a row turned out to be against a build that predated the fix,
# and there was no way to tell from a screenshot. Now there is. Absent git is not an error --
# a source drop still builds, it just says "local".
find_package(Git QUIET)
set(TSRO_BUILD_ID "local")
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                  OUTPUT_VARIABLE TSRO_GIT_SHA
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET
                  RESULT_VARIABLE TSRO_GIT_RESULT)
  if(TSRO_GIT_RESULT EQUAL 0 AND TSRO_GIT_SHA)
    set(TSRO_BUILD_ID "${TSRO_GIT_SHA}")
  endif()
endif()
message(STATUS "TeamSpeak overlay build id: ${TSRO_BUILD_ID}")

target_compile_definitions(tsro_shared PUBLIC
  TSRO_VERSION="${PROJECT_VERSION}"
  TSRO_BUILD_ID="${TSRO_BUILD_ID}")
