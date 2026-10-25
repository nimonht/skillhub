# Removes exactly the files that `cmake --install` recorded in install_manifest.txt. Directories are never removed, so
# shared ones (share/applications, the hicolor icon theme) survive. Run by the `uninstall` target from the build directory;
# with DESTDIR set, the files below that root are removed.
set(manifest "${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt")
if(NOT EXISTS "${manifest}")
    message(FATAL_ERROR "${manifest} not found: nothing was installed from this build directory")
endif()

file(STRINGS "${manifest}" files)
foreach(file IN LISTS files)
    set(path "$ENV{DESTDIR}${file}")
    if(IS_SYMLINK "${path}" OR EXISTS "${path}")
        if(IS_DIRECTORY "${path}" AND NOT IS_SYMLINK "${path}")
            message(STATUS "Keeping directory ${path}")
            continue()
        endif()
        file(REMOVE "${path}")
        if(IS_SYMLINK "${path}" OR EXISTS "${path}")
            message(SEND_ERROR "Could not remove ${path}")
        else()
            message(STATUS "Removed ${path}")
        endif()
    endif()
endforeach()
