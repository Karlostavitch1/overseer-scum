cmake_minimum_required(VERSION 3.24)
project(Overseer LANGUAGES C CXX VERSION 0.1.0)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Overseer native components target Windows x64 only (SCUMServer.exe).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so server owners don't need the VC++ redistributable. The policy
# default makes third-party subprojects honour it too.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

include(FetchContent)
# MinHook (BSD-2-Clause) — inline function hooking. Pinned to v1.3.4.
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537)
FetchContent_MakeAvailable(minhook)

set(OVERSEER_DEFINITIONS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set(OVERSEER_OPTIONS /W4 /permissive- /utf-8)

# --- Core library: everything except DllMain, shared with the tests ----------
add_library(overseer_core STATIC
    src/core/bootstrap.cpp
    src/core/log.cpp
    src/core/dotnet_host.cpp
    src/engine/null_engine_bridge.cpp
    src/engine/scum_engine_bridge.cpp
    src/util/pattern.cpp
    src/util/module.cpp
    src/util/hook.cpp
    src/util/safe_memory.cpp
    src/ue4/reflection.cpp
    src/ue4/probe.cpp
)
target_include_directories(overseer_core PUBLIC include src)
target_compile_definitions(overseer_core PUBLIC ${OVERSEER_DEFINITIONS}
    OVERSEER_VERSION="${PROJECT_VERSION}")
target_compile_options(overseer_core PRIVATE ${OVERSEER_OPTIONS})
target_link_libraries(overseer_core PUBLIC minhook)

# --- overseer_native.dll ------------------------------------------------------
add_library(overseer_native SHARED src/core/dllmain.cpp)
target_link_libraries(overseer_native PRIVATE overseer_core)
target_compile_options(overseer_native PRIVATE ${OVERSEER_OPTIONS})

# --- Proxy: version.dll loaded by SCUMServer.exe, loads overseer_native ------
add_library(overseer_proxy SHARED proxy/version_proxy.cpp)
set_target_properties(overseer_proxy PROPERTIES OUTPUT_NAME "version")
target_compile_definitions(overseer_proxy PRIVATE ${OVERSEER_DEFINITIONS})
target_compile_options(overseer_proxy PRIVATE ${OVERSEER_OPTIONS})

# --- Tests --------------------------------------------------------------------
enable_testing()
add_executable(overseer_native_tests tests/test_main.cpp)
target_link_libraries(overseer_native_tests PRIVATE overseer_core)
target_compile_options(overseer_native_tests PRIVATE ${OVERSEER_OPTIONS})
add_test(NAME overseer_native_tests COMMAND overseer_native_tests)

install(TARGETS overseer_native RUNTIME DESTINATION overseer/bin)
install(TARGETS overseer_proxy RUNTIME DESTINATION .)
