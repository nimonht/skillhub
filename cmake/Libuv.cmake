# libuv (MIT) for `skillhub serve`: its event loop, timers and file-system events. Linux builds use the system library
# (libuv1-dev, libuv-devel, libuv), as distributions require. macOS and Windows build a pinned release and link it
# statically, so the program needs nothing installed next to it. That build installs into a private folder below the
# build tree, so none of libuv's own files end up in `cmake --install`.
#
# Defines the target skillhub::uv.
#   -DSKILLHUB_SYSTEM_LIBUV=ON|OFF   use the system libuv (default: ON on Linux, OFF elsewhere)
#   -DSKILLHUB_LIBUV_TARBALL=PATH    a downloaded copy of the release archive, for builds without network access

set(SKILLHUB_LIBUV_VERSION 1.51.0)
set(SKILLHUB_LIBUV_SHA256 27e55cf7083913bfb6826ca78cde9de7647cded648d35f24163f2d31bb9f51cd)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_skillhub_uv_default ON)
else()
    set(_skillhub_uv_default OFF)
endif()
option(SKILLHUB_SYSTEM_LIBUV "Use the system libuv instead of building the pinned release" ${_skillhub_uv_default})
set(SKILLHUB_LIBUV_TARBALL "" CACHE FILEPATH "A downloaded copy of the libuv release archive (no download then)")

if(SKILLHUB_SYSTEM_LIBUV)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv>=1.44)
    add_library(skillhub::uv ALIAS PkgConfig::LIBUV)
    return()
endif()

include(ExternalProject)
set(_skillhub_uv_prefix "${CMAKE_BINARY_DIR}/_deps/libuv-install")
if(WIN32)
    set(_skillhub_uv_lib "${_skillhub_uv_prefix}/lib/libuv${CMAKE_STATIC_LIBRARY_SUFFIX}")  # libuv names it libuv.lib
else()
    set(_skillhub_uv_lib "${_skillhub_uv_prefix}/lib/libuv.a")
endif()
if(SKILLHUB_LIBUV_TARBALL)
    set(_skillhub_uv_url "${SKILLHUB_LIBUV_TARBALL}")
else()
    set(_skillhub_uv_url "https://github.com/libuv/libuv/archive/refs/tags/v${SKILLHUB_LIBUV_VERSION}.tar.gz")
endif()
string(REPLACE ";" "|" _skillhub_uv_archs "${CMAKE_OSX_ARCHITECTURES}")  # a universal build: x86_64|arm64

ExternalProject_Add(skillhub_libuv
    URL "${_skillhub_uv_url}"
    URL_HASH SHA256=${SKILLHUB_LIBUV_SHA256}
    DOWNLOAD_NAME "libuv-${SKILLHUB_LIBUV_VERSION}.tar.gz"
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps"
    PREFIX "${CMAKE_BINARY_DIR}/_deps/libuv"
    LIST_SEPARATOR |
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${_skillhub_uv_prefix}
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_OSX_ARCHITECTURES=${_skillhub_uv_archs}
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DLIBUV_BUILD_SHARED=OFF
        -DLIBUV_BUILD_TESTS=OFF
        -DLIBUV_BUILD_BENCH=OFF
    BUILD_BYPRODUCTS "${_skillhub_uv_lib}")

file(MAKE_DIRECTORY "${_skillhub_uv_prefix}/include")  # an imported target's include folder must exist when configuring
add_library(skillhub_uv STATIC IMPORTED GLOBAL)
set_target_properties(skillhub_uv PROPERTIES
    IMPORTED_LOCATION "${_skillhub_uv_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_skillhub_uv_prefix}/include")
add_dependencies(skillhub_uv skillhub_libuv)
if(WIN32)
    set_property(TARGET skillhub_uv APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                 psapi user32 advapi32 iphlpapi userenv ws2_32 dbghelp ole32 shell32)
else()
    find_package(Threads REQUIRED)
    set_property(TARGET skillhub_uv APPEND PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads)
endif()
add_library(skillhub::uv ALIAS skillhub_uv)
