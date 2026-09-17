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

target_compile_definitions(tsro_shared PUBLIC TSRO_VERSION="${PROJECT_VERSION}")
