cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_CXX_MODULE_STD ON)

project(exon LANGUAGES CXX)

set(EXON_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(TOMLCPP_DIR "" CACHE PATH "Path to tomlcpp source")

add_subdirectory(${TOMLCPP_DIR} tomlcpp)

add_executable(exon)
target_sources(exon PRIVATE
    ${EXON_SOURCE_DIR}/src/main.cpp
)
target_sources(exon
    PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${EXON_SOURCE_DIR} FILES
    ${EXON_SOURCE_DIR}/src/manifest.cppm
    ${EXON_SOURCE_DIR}/src/toolchain.cppm
    ${EXON_SOURCE_DIR}/src/lock.cppm
    ${EXON_SOURCE_DIR}/src/semver.cppm
    ${EXON_SOURCE_DIR}/src/fetch.cppm
    ${EXON_SOURCE_DIR}/src/build.cppm
    ${EXON_SOURCE_DIR}/src/templates.cppm
    ${EXON_SOURCE_DIR}/src/commands.cppm
)
target_link_libraries(exon PRIVATE tomlcpp)
