cmake_minimum_required(VERSION 3.20)

project(goose_overlay VERSION 0.5.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "GooseOverlay currently supports Windows only")
endif()

add_library(bfg_overlay_win64 SHARED src/plugin.cpp)
target_compile_features(bfg_overlay_win64 PRIVATE cxx_std_17)
target_include_directories(bfg_overlay_win64 PRIVATE third_party/ts3client-pluginsdk/include)
target_compile_definitions(bfg_overlay_win64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(bfg_overlay_win64 PRIVATE user32 gdi32)

if(MSVC)
    target_compile_options(bfg_overlay_win64 PRIVATE /utf-8 /W4 /EHsc)
endif()

set_target_properties(bfg_overlay_win64 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "bfg_overlay_win64"
)
