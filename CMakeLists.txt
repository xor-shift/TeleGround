cmake_minimum_required(VERSION 3.23)
project(TeleGround)

set(CMAKE_CXX_STANDARD 23)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(${CMAKE_CXX_FLAGS} "${CMAKE_CXX_FLAGS} -stdlib=libc++ -fcoroutines")
endif ()

set(${CMAKE_CXX_FLAGS} "${CMAKE_CXX_FLAGS} -fsanitize=address -fsanitize=undefined")
set(${CMAKE_EXE_LINKER_FLAGS} "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -fsanitize=undefined")

set(IMGUI_SFML_FIND_SFML OFF)
set(IMGUI_DIR ../imgui)

set(LibStuffCompileBenchmarks OFF)
set(LibStuffCompileTests OFF)
set(LibStuffCompileExamples OFF)

add_subdirectory(Thirdparty/Crow)
add_subdirectory(Thirdparty/imgui-sfml)
add_subdirectory(Thirdparty/LibStuff)
add_subdirectory(Thirdparty/SFML)

add_executable(TeleGround main.cpp)

target_link_libraries(TeleGround libstuff ImGui-SFML Crow)
