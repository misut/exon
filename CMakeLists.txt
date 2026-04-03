cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_CXX_MODULE_STD ON)

set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "")
add_compile_definitions(_LIBCPP_DISABLE_AVAILABILITY)

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

add_library(lock)
target_sources(lock
    PUBLIC FILE_SET CXX_MODULES FILES
    src/lock.cppm
)
target_link_libraries(lock PUBLIC tomlcpp)

add_library(semver)
target_sources(semver
    PUBLIC FILE_SET CXX_MODULES FILES
    src/semver.cppm
)

add_library(fetch)
target_sources(fetch
    PUBLIC FILE_SET CXX_MODULES FILES
    src/fetch.cppm
)
target_link_libraries(fetch PUBLIC manifest toolchain lock semver)

add_library(build)
target_sources(build
    PUBLIC FILE_SET CXX_MODULES FILES
    src/build.cppm
)
target_link_libraries(build PUBLIC manifest toolchain fetch)

add_library(templates)
target_sources(templates
    PUBLIC FILE_SET CXX_MODULES FILES
    src/templates.cppm
)

add_executable(exon src/main.cpp)
target_link_libraries(exon PRIVATE build templates)
