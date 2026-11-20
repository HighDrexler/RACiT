# Tests that need no game data: cmake -S tests -B build/tests && cmake --build build/tests
# && ctest --test-dir build/tests --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(racit_tests CXX)
enable_testing()
add_executable(desktop_mouse_test desktop_mouse_test.cpp ../game/desktop_mouse.cpp)
target_compile_features(desktop_mouse_test PRIVATE cxx_std_11)
add_test(NAME desktop_mouse COMMAND desktop_mouse_test)
