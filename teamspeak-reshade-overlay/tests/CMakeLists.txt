add_executable(tsro_tests
  main.cpp
  test_json.cpp
  test_protocol.cpp
  test_framing.cpp
  test_state_store.cpp
  test_config.cpp
  test_layout.cpp
  test_notifications.cpp
  test_triple_buffer.cpp
  test_ipc_integration.cpp
  test_plugin_core.cpp
  # The plugin's event collection and state machine are platform-independent and are tested
  # against a scripted TeamSpeak client, so no SDK or TeamSpeak install is needed here.
  ${CMAKE_SOURCE_DIR}/teamspeak-plugin/src/plugin_core.cpp
  ${CMAKE_SOURCE_DIR}/teamspeak-plugin/src/ts_state.cpp
  ${CMAKE_SOURCE_DIR}/teamspeak-plugin/src/ts_query_stub.cpp
)

target_link_libraries(tsro_tests PRIVATE tsro_shared)
target_include_directories(tsro_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/teamspeak-plugin/include
)

# One ctest entry per suite, so a failure names the area rather than just "tests failed".
foreach(suite json protocol framing state_store config layout notifications triple_buffer ipc plugin)
  add_test(NAME ${suite} COMMAND tsro_tests ${suite})
endforeach()
