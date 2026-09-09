cmake_minimum_required(VERSION 3.20)
project(minimon VERSION 0.8.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(minimon
  src/main.cpp
  src/cpu_sampler.cpp
  src/gpu_sampler.cpp
  src/renderer.cpp
  src/app.rc
)

target_include_directories(minimon PRIVATE src)

# Win32 APIs: PDH + DXGI + IP Helper
target_link_libraries(minimon PRIVATE pdh dxgi iphlpapi ole32)

if (MINGW)
  target_compile_options(minimon PRIVATE -Wall -Wextra -fexec-charset=UTF-8 -finput-charset=UTF-8)
endif()
