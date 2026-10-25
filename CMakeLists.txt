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

add_library(skillhub_core STATIC
    src/core/sh_exe.c src/core/sh_exe.h)
target_include_directories(skillhub_core PUBLIC src/core)
target_compile_options(skillhub_core PRIVATE ${SKILLHUB_C_WARNINGS})
target_compile_definitions(skillhub_core PUBLIC SKILLHUB_VERSION="${SKILLHUB_VERSION}")

function(skillhub_cli_target target exe)
    target_link_libraries(${target} PRIVATE skillhub_core)
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

# ---- window -------------------------------------------------------------------------------------------------------------

if(SKILLHUB_GUI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Concurrent)
    qt_standard_project_setup()
    set(CMAKE_AUTORCC ON)

    qt_add_executable(skillhub_gui WIN32 MACOSX_BUNDLE
        src/main.cpp src/store.cpp src/providers.cpp src/transfer.cpp src/platform.cpp src/app.cpp
        src/store.h src/platform.h src/app.h
        resources.qrc)
    target_link_libraries(skillhub_gui PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Concurrent)
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
