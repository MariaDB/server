CMAKE_MINIMUM_REQUIRED(VERSION 3.12.0)

project(MIR)

add_library(mir STATIC
    mir.c
    mir-gen.c
    c2mir/c2mir.c
)

target_include_directories(mir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})