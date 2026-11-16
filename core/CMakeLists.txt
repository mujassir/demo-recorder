add_library(dr_core STATIC
  src/clock.cpp
  src/dr_core.cpp
  src/file_util.cpp
  src/input_log.cpp
  src/json.cpp
  src/project.cpp
)
add_library(DemoRecorder::core ALIAS dr_core)
target_include_directories(dr_core PUBLIC include)
target_compile_features(dr_core PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(dr_core PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(dr_core PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(dr_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(dr_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

# Crash-recovery tool (spec P1: "crash-recovery tool"; started in P0).
add_executable(dr_recover tools/dr_recover.cpp)
target_link_libraries(dr_recover PRIVATE dr_core)

if(DR_BUILD_TESTS)
  add_executable(dr_core_tests
    tests/main.cpp
    tests/test_c_api.cpp
    tests/test_clock.cpp
    tests/test_frame_pacer.cpp
    tests/test_input_log.cpp
    tests/test_json.cpp
    tests/test_project.cpp
    tests/test_spsc_ring.cpp
  )
  target_link_libraries(dr_core_tests PRIVATE dr_core)
  target_compile_definitions(dr_core_tests PRIVATE
    DR_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
  add_test(NAME dr_core_tests COMMAND dr_core_tests)
endif()

if(DR_BUILD_FUZZERS)
  foreach(target json project cursor_log)
    add_executable(fuzz_${target} fuzz/fuzz_${target}.cpp)
    target_link_libraries(fuzz_${target} PRIVATE dr_core)
    target_compile_options(fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
  target_compile_options(dr_core PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
  target_link_options(dr_core PUBLIC -fsanitize=address,undefined)
endif()
