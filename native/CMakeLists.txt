cmake_minimum_required(VERSION 3.24)
project(Overseer LANGUAGES CXX VERSION 0.1.0)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Overseer native components target Windows x64 only (SCUMServer.exe).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so server owners don't need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
endif()

# --- Core: bootstrap, logging, .NET hosting, engine bridge -------------------
add_library(overseer_native SHARED
    src/core/dllmain.cpp
    src/core/bootstrap.cpp
    src/core/log.cpp
    src/core/dotnet_host.cpp
    src/engine/null_engine_bridge.cpp
)
target_include_directories(overseer_native PRIVATE include src)
target_compile_definitions(overseer_native PRIVATE OVERSEER_VERSION="${PROJECT_VERSION}")

# --- Proxy: version.dll loaded by SCUMServer.exe, loads overseer_native ------
add_library(overseer_proxy SHARED proxy/version_proxy.cpp)
set_target_properties(overseer_proxy PROPERTIES OUTPUT_NAME "version")

install(TARGETS overseer_native RUNTIME DESTINATION overseer/bin)
install(TARGETS overseer_proxy RUNTIME DESTINATION .)
