cmake_minimum_required(VERSION 3.25)

project(SysRecord VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
    add_compile_options(/Zc:__cplusplus)
endif()

include(FetchContent)
include(CTest)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(mp3lame CONFIG REQUIRED)

if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

set(FLTK_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FLTK_MSVC_RUNTIME_DLL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    fltk
    GIT_REPOSITORY https://github.com/fltk/fltk.git
    GIT_TAG release-1.4.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fltk)

add_library(sys_record_core STATIC
    src/audio/mp3_encoder.cpp
    src/audio/recorder.cpp
    src/config/recording_config.cpp
)
target_include_directories(sys_record_core PUBLIC src)
target_link_libraries(sys_record_core PUBLIC
    nlohmann_json::nlohmann_json
    mp3lame::mp3lame
)

add_executable(SysRecord WIN32
    src/main.cpp
    src/app.rc
)
target_link_libraries(SysRecord PRIVATE
    fltk::fltk
    sys_record_core
    spdlog::spdlog
    comctl32
    shell32
    user32
    gdi32
    ole32
)

if(MSVC)
    target_compile_options(sys_record_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(SysRecord PRIVATE /W4 /permissive- /utf-8)
endif()

set_target_properties(SysRecord PROPERTIES
    OUTPUT_NAME "SysRecord"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:SysRecord>"
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
