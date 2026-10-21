add_executable(ode_smoke main.cpp)
target_link_libraries(ode_smoke PRIVATE ode)
add_test(NAME ode_smoke COMMAND ode_smoke)
