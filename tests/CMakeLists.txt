# Tests that need no game data: cmake -S tests -B build/tests && cmake --build build/tests
# && ctest --test-dir build/tests --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(racit_tests CXX)
enable_testing()
add_executable(skip_puzzles_test skip_puzzles_test.cpp ../game/skip_puzzles.cpp)
target_compile_features(skip_puzzles_test PRIVATE cxx_std_11)
add_test(NAME skip_puzzles COMMAND skip_puzzles_test)
