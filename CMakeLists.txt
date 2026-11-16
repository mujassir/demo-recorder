# Top-level build. The shared core builds on macOS, Windows and Linux; the
# Windows engine is added on Windows. The macOS app is built by Xcode
# (see macos/README.md) and compiles the core sources directly.
cmake_minimum_required(VERSION 3.24)
project(DemoRecorder LANGUAGES C CXX)

option(DR_BUILD_TESTS "Build core unit tests" ON)
option(DR_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
option(DR_BUILD_WINDOWS_APP "Build the Windows engine and recorder spike" ${WIN32})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(DR_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)

if(WIN32 AND DR_BUILD_WINDOWS_APP)
  add_subdirectory(windows)
endif()
