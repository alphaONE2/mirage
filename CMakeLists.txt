cmake_minimum_required(VERSION 4.0)
project(mirage)

set(CMAKE_CXX_STANDARD 26)

add_library(mirage SHARED)
set_target_properties(mirage PROPERTIES
        SUFFIX .addon
        PREFIX "")
target_include_directories(mirage SYSTEM AFTER PRIVATE
        extern/reshade/include
        extern/imgui
        extern/ini.h)
target_compile_definitions(mirage PRIVATE UNICODE _UNICODE)
target_link_options(mirage BEFORE PRIVATE -static)

target_link_libraries(mirage PRIVATE
        d3d11
        dxgi)

target_sources(mirage PRIVATE
        mirage.cpp)
