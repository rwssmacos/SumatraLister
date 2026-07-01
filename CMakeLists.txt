cmake_minimum_required(VERSION 3.15)
project(SumatraLister CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This plugin only builds for Windows (it is a Total Commander WLX plugin).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(SumatraLister SHARED
    SumatraLister.cpp
    SumatraLister.def
)

target_link_libraries(SumatraLister PRIVATE shlwapi user32 advapi32 shell32)

# Produce SumatraLister.wlx for 32-bit builds and SumatraLister.wlx64 for
# 64-bit builds, matching Total Commander's plugin-bitness naming convention.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(SumatraLister PROPERTIES
        SUFFIX ".wlx64"
        OUTPUT_NAME "SumatraLister")
else()
    set_target_properties(SumatraLister PROPERTIES
        SUFFIX ".wlx"
        OUTPUT_NAME "SumatraLister")
endif()
