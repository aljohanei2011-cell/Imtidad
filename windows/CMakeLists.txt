cmake_minimum_required(VERSION 3.20)
project(screenlink_host LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SCREENLINK_BUILD_TESTS "Build portable protocol unit tests" OFF)

# Protocol logic has no Windows dependency, so it can also be tested on other systems.
add_library(screenlink_protocol STATIC src/protocol.cpp src/input.cpp)
target_include_directories(screenlink_protocol PUBLIC src third_party)

if(WIN32)
  add_executable(Imtidad WIN32 src/main.cpp src/stream.cpp src/display.cpp src/audio.cpp src/ui.cpp src/mixer.cpp src/app.rc)
  target_link_libraries(Imtidad PRIVATE screenlink_protocol ws2_32 d3d11 dxgi mfplat mfuuid wmcodecdspuuid ole32 oleaut32 user32 shell32 advapi32 ksuser gdiplus dwmapi shlwapi uuid iphlpapi bcrypt crypt32)
  if(MSVC)
    target_compile_options(Imtidad PRIVATE /W4 /utf-8)
    target_link_options(Imtidad PRIVATE /ENTRY:wWinMainCRTStartup)
    target_compile_options(screenlink_protocol PRIVATE /W4 /utf-8)
  endif()
endif()

if(SCREENLINK_BUILD_TESTS)
  enable_testing()
  add_executable(protocol_test tests/protocol_test.cpp)
  target_link_libraries(protocol_test PRIVATE screenlink_protocol)
  add_test(NAME protocol_test COMMAND protocol_test)
endif()
