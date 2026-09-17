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
)

target_link_libraries(tsro_tests PRIVATE tsro_shared)
target_include_directories(tsro_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# One ctest entry per suite, so a failure names the area rather than just "tests failed".
foreach(suite json protocol framing state_store config layout notifications triple_buffer ipc)
  add_test(NAME ${suite} COMMAND tsro_tests ${suite})
endforeach()
