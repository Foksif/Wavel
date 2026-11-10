cmake_minimum_required(VERSION 3.25)

project(wavel VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
pkg_check_modules(LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER SOURCES EXCLUDE REGEX "/build/")

# ---- GResource ----
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

set(RES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/resources)
set(RES_C   ${CMAKE_CURRENT_BINARY_DIR}/resources.c)

add_custom_command(
    OUTPUT ${RES_C}
    COMMAND ${GLIB_COMPILE_RESOURCES}
            --target=${RES_C}
            --sourcedir=${RES_DIR}
            --generate-source
            ${RES_DIR}/wavel.gresource.xml
    DEPENDS ${RES_DIR}/wavel.gresource.xml
            ${RES_DIR}/wavel.css
    VERBATIM
)

add_executable(wavel ${SOURCES} ${RES_C})

target_include_directories(wavel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(wavel PRIVATE
    PkgConfig::GTK4
    PkgConfig::GTKMM
    PkgConfig::LAYER_SHELL
)

find_package(nlohmann_json REQUIRED)

target_link_libraries(wavel PRIVATE
    nlohmann_json::nlohmann_json
)

pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

target_link_libraries(wavel PRIVATE PkgConfig::PIPEWIRE)

target_compile_options(wavel PRIVATE -Wall -Wextra -Wpedantic)
