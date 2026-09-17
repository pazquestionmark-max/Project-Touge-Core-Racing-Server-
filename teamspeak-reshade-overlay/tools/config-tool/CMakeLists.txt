add_executable(tsro_config main.cpp)
target_link_libraries(tsro_config PRIVATE tsro_shared)
set_target_properties(tsro_config PROPERTIES OUTPUT_NAME "tsro-config")
install(TARGETS tsro_config RUNTIME DESTINATION tools)
