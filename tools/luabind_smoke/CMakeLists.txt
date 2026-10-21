add_executable(luabind_smoke main.cpp)
target_link_libraries(luabind_smoke PRIVATE luabind)
add_test(NAME luabind_smoke COMMAND luabind_smoke)
