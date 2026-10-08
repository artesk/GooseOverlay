cmake_minimum_required(VERSION 3.20)

project(goose_overlay VERSION 0.13.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "GooseOverlay currently supports Windows only")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(GOOSEOVERLAY_PLATFORM "win64")
else()
    set(GOOSEOVERLAY_PLATFORM "win32")
endif()

add_library(goose_overlay SHARED src/plugin.cpp src/plugin.def src/portraits.rc)
target_compile_features(goose_overlay PRIVATE cxx_std_17)
target_include_directories(goose_overlay PRIVATE src third_party/ts3client-pluginsdk/include)
target_compile_definitions(goose_overlay PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(goose_overlay PRIVATE user32 gdi32 gdiplus ole32 advapi32)

if(MSVC)
    target_compile_options(goose_overlay PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET goose_overlay PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(goose_overlay PROPERTIES
    PREFIX ""
    OUTPUT_NAME "bfg_overlay_${GOOSEOVERLAY_PLATFORM}"
)
