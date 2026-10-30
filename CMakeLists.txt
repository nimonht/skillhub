# Skill Hub: a C command line (src/cli, src/core) and a Qt Quick window (skillhub-gui; skillhub.exe on Windows).
#   cmake -B build && cmake --build build
#   cmake --install build        # Linux: below ~/.local by default; -DCMAKE_INSTALL_PREFIX=/usr for a system-wide install
#   cmake --build build --target uninstall    # removes exactly the files the install wrote, never shared directories
cmake_minimum_required(VERSION 3.22)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0 CACHE STRING "Oldest macOS the program runs on")
endif()

project(skillhub VERSION 2.0.0
        DESCRIPTION "One skills folder for every coding agent"
        HOMEPAGE_URL "https://github.com/nimonht/skillhub"
        LANGUAGES C CXX)

set(SKILLHUB_VERSION_SUFFIX "-dev" CACHE STRING "Appended to the version number (empty for a release)")
set(SKILLHUB_VERSION "${PROJECT_VERSION}${SKILLHUB_VERSION_SUFFIX}")

option(SKILLHUB_GUI "Build the window (skillhub-gui), which needs Qt 6.4 or newer" ON)
option(SKILLHUB_WERROR "Treat compiler warnings as errors (CI)" OFF)
option(SKILLHUB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (CI, debugging)" OFF)
option(SKILLHUB_FUZZ "Build the libFuzzer targets in tests/fuzz (clang only)" OFF)
if(SKILLHUB_FUZZ)
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
elseif(SKILLHUB_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

if(UNIX AND NOT APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
endif()
include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)  # C99 printf (%zu, %lld) instead of the old msvcrt one
endif()

if(MSVC)
    # the C runtime's "use the _s variant" warnings are not useful in portable code that checks its own bounds
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    set(SKILLHUB_C_WARNINGS /W4 /utf-8)
    if(SKILLHUB_WERROR)
        list(APPEND SKILLHUB_C_WARNINGS /WX)
    endif()
else()
    set(SKILLHUB_C_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)
    if(SKILLHUB_WERROR)
        list(APPEND SKILLHUB_C_WARNINGS -Werror)
    endif()
endif()

# Windows resources: icon and version information for each executable
function(skillhub_windows_resources target exe description)
    set(SKILLHUB_EXE "${exe}")
    set(SKILLHUB_EXE_DESCRIPTION "${description}")
    set(SKILLHUB_ICO "${CMAKE_SOURCE_DIR}/branding/favicon.ico")
    configure_file(${CMAKE_SOURCE_DIR}/data/windows/skillhub.rc.in ${CMAKE_BINARY_DIR}/${exe}.rc @ONLY)
    target_sources(${target} PRIVATE ${CMAKE_BINARY_DIR}/${exe}.rc)
endfunction()

# ---- C core and command line --------------------------------------------------------------------------------------------

# yyjson (third_party/yyjson, MIT): built as it comes, without our warning flags
add_library(skillhub_yyjson STATIC third_party/yyjson/yyjson.c third_party/yyjson/yyjson.h)
target_include_directories(skillhub_yyjson PUBLIC third_party/yyjson)

# the agent catalog is compiled into the program (1.x read it from a Qt resource)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/providers_json.c
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/data/providers.json -DOUTPUT=${CMAKE_BINARY_DIR}/providers_json.c
            -DNAME=sh_providers_json -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/data/providers.json ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "Embedding the agent catalog"
    VERBATIM)

add_library(skillhub_core STATIC
    src/core/sh_catalog.c
    src/core/sh_collate.c src/core/sh_collate.h
    src/core/sh_engine.c src/core/sh_engine.h
    src/core/sh_exe.c src/core/sh_exe.h
    src/core/sh_frontmatter.c src/core/sh_frontmatter.h
    src/core/sh_fs.c src/core/sh_fs.h
    src/core/sh_git.c src/core/sh_git.h
    src/core/sh_hash.c src/core/sh_hash.h
    src/core/sh_json.c src/core/sh_json.h
    src/core/sh_link.c src/core/sh_link.h
    src/core/sh_lock.c
    src/core/sh_origin.c src/core/sh_origin.h
    src/core/sh_path.c src/core/sh_path.h
    src/core/sh_proc.c src/core/sh_proc.h
    src/core/sh_rpc.c src/core/sh_rpc.h
    src/core/sh_store.c src/core/sh_store.h
    src/core/sh_str.c src/core/sh_str.h
    src/core/sh_thread.c src/core/sh_thread.h
    src/core/sh_time.c src/core/sh_time.h
    src/core/sh_transfer.c
    src/core/sh_units.c
    src/core/sh_utf.c src/core/sh_utf.h src/core/sh_unicode_tables.h
    ${CMAKE_BINARY_DIR}/providers_json.c)
target_include_directories(skillhub_core PUBLIC src/core)
target_compile_options(skillhub_core PRIVATE ${SKILLHUB_C_WARNINGS})
target_compile_definitions(skillhub_core PUBLIC SKILLHUB_VERSION="${SKILLHUB_VERSION}")
find_package(Threads REQUIRED)
target_link_libraries(skillhub_core PUBLIC skillhub_yyjson Threads::Threads)
if(WIN32)
    target_link_libraries(skillhub_core PUBLIC shell32 ole32 uuid)
endif()

# `skillhub serve` (src/serve): the backend the window talks to. The only part that needs libuv, so it is linked into the
# command-line programs and nowhere else.
include(cmake/Libuv.cmake)
add_library(skillhub_serve STATIC
    src/serve/jobs.c
    src/serve/serve.c src/serve/serve.h src/serve/sh_serve.h
    src/serve/state.c
    src/serve/watch.c)
target_include_directories(skillhub_serve PUBLIC src/serve)
target_compile_options(skillhub_serve PRIVATE ${SKILLHUB_C_WARNINGS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(skillhub_serve PRIVATE _DEFAULT_SOURCE)  # libuv's headers use POSIX types
endif()
target_link_libraries(skillhub_serve PUBLIC skillhub_core skillhub::uv)

function(skillhub_cli_target target exe)
    target_link_libraries(${target} PRIVATE skillhub_serve)
    target_compile_options(${target} PRIVATE ${SKILLHUB_C_WARNINGS})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${exe})
    if(WIN32)
        # UTF-8 as the process code page and paths longer than 260 characters
        target_sources(${target} PRIVATE data/windows/skillhub.manifest)
        skillhub_windows_resources(${target} ${exe} "Skill Hub command line")
        if(MINGW)
            target_link_options(${target} PRIVATE -municode)
        endif()
    endif()
endfunction()

add_executable(skillhub_cli src/cli/main.c)
if(WIN32)
    skillhub_cli_target(skillhub_cli skillhub-cli)
    # The same program without a console window: scheduled tasks and the window's background service run this one,
    # so nothing flashes on screen.
    add_executable(skillhub_cliw WIN32 src/cli/main.c)
    skillhub_cli_target(skillhub_cliw skillhubw)
    if(MSVC)
        target_link_options(skillhub_cliw PRIVATE /ENTRY:wmainCRTStartup)
    endif()
else()
    skillhub_cli_target(skillhub_cli skillhub)
endif()

if(UNIX AND NOT APPLE)
    install(TARGETS skillhub_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(SKILLHUB_FUZZ)
    foreach(target frontmatter import source lock rpc)
        add_executable(fuzz_${target} tests/fuzz/fuzz_${target}.c)
        target_link_libraries(fuzz_${target} PRIVATE skillhub_core)
        target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
    endforeach()
endif()

option(SKILLHUB_TESTS "Build the C unit tests (ctest)" ON)
if(SKILLHUB_TESTS)
    enable_testing()
    add_executable(skillhub_unit_tests tests/unit/test_main.c tests/unit/test.h tests/unit/test_text.c tests/unit/test_paths.c
                                       tests/unit/test_frontmatter.c tests/unit/test_lock.c tests/unit/test_git.c
                                       tests/unit/test_store.c)
    target_link_libraries(skillhub_unit_tests PRIVATE skillhub_core)
    target_compile_options(skillhub_unit_tests PRIVATE ${SKILLHUB_C_WARNINGS})
    target_compile_definitions(skillhub_unit_tests PRIVATE SKILLHUB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME unit COMMAND skillhub_unit_tests)
endif()

# ---- window -------------------------------------------------------------------------------------------------------------

if(SKILLHUB_GUI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
    qt_standard_project_setup()
    set(CMAKE_AUTORCC ON)

    qt_add_executable(skillhub_gui WIN32 MACOSX_BUNDLE
        src/main.cpp src/app.cpp src/app.h
        src/gui/core_client.cpp src/gui/core_client.h src/gui/skill_row.h
        resources.qrc)
    # the window runs the command line as its backend: build it too, so a fresh build starts
    add_dependencies(skillhub_gui skillhub_cli)
    if(WIN32)
        add_dependencies(skillhub_gui skillhub_cliw)
    endif()
    target_link_libraries(skillhub_gui PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
    target_compile_definitions(skillhub_gui PRIVATE SKILLHUB_VERSION="${SKILLHUB_VERSION}")
    set_target_properties(skillhub_gui PROPERTIES OUTPUT_NAME skillhub-gui)
    if(MSVC)
        target_compile_options(skillhub_gui PRIVATE /utf-8)
    endif()

    if(WIN32)
        set_target_properties(skillhub_gui PROPERTIES OUTPUT_NAME skillhub)
        skillhub_windows_resources(skillhub_gui skillhub "Skill Hub")
    endif()

    if(APPLE)
        set_source_files_properties(data/skillhub.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(skillhub_gui PRIVATE data/skillhub.icns)
        set_target_properties(skillhub_gui PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/data/macos/Info.plist
            MACOSX_BUNDLE_ICON_FILE skillhub.icns
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
            MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
        # The command line lives inside the bundle, next to the window: Contents/MacOS/skillhub
        add_dependencies(skillhub_gui skillhub_cli)
        add_custom_command(TARGET skillhub_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:skillhub_cli> $<TARGET_BUNDLE_CONTENT_DIR:skillhub_gui>/MacOS/skillhub
            VERBATIM)
    endif()

    if(UNIX AND NOT APPLE)
        install(TARGETS skillhub_gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES data/skillhub.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES data/io.github.nimonht.skillhub.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
        # one rule per icon file: uninstall then removes only these files, never the shared hicolor directories
        foreach(size 16 24 32 48 64 128 256 512)
            install(FILES data/icons/hicolor/${size}x${size}/apps/skillhub.png
                    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps)
        endforeach()
        install(FILES data/icons/hicolor/scalable/apps/skillhub.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    endif()
endif()

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/Uninstall.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Removing the files listed in install_manifest.txt"
    VERBATIM)
