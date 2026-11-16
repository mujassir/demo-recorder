# Windows engine + P0 recorder spike. Build from the repo root:
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
# The generated build/DemoRecorder.sln opens in Visual Studio.

if(NOT MSVC)
  message(FATAL_ERROR "The Windows engine requires MSVC (C++/WinRT)")
endif()

add_library(dr_win_engine STATIC
  engine/src/capture.cpp
  engine/src/cursor.cpp
  engine/src/encoder.cpp
  engine/src/gpu.cpp
  engine/src/recorder.cpp
  engine/src/util.cpp
)
target_include_directories(dr_win_engine PUBLIC engine/include PRIVATE engine/src)
target_link_libraries(dr_win_engine
  PUBLIC dr_core
  PRIVATE d3d11 dxgi mfplat mfreadwrite mfuuid windowsapp shcore)
target_compile_definitions(dr_win_engine PUBLIC
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(dr_win_engine PRIVATE /W4 /permissive- /utf-8 /bigobj /EHsc)

add_executable(drrec recorder_cli/main.cpp recorder_cli/drrec.manifest)
target_link_libraries(drrec PRIVATE dr_win_engine)
target_compile_options(drrec PRIVATE /W4 /permissive- /utf-8 /EHsc)
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT drrec)
