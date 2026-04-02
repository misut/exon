cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_CXX_MODULE_STD ON)

project(exon LANGUAGES CXX)

add_subdirectory(tomlcpp)

add_library(manifest)
target_sources(manifest
    PUBLIC FILE_SET CXX_MODULES FILES
    src/manifest.cppm
)
target_link_libraries(manifest PUBLIC tomlcpp)

add_library(toolchain)
target_sources(toolchain
    PUBLIC FILE_SET CXX_MODULES FILES
    src/toolchain.cppm
)

add_library(build)
target_sources(build
    PUBLIC FILE_SET CXX_MODULES FILES
    src/build.cppm
)
target_link_libraries(build PUBLIC manifest toolchain)

add_executable(exon src/main.cpp)
target_link_libraries(exon PRIVATE build)
