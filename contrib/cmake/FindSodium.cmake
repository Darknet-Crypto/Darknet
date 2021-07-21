# Written in 2016 by Henrik Steffen Gaßmann <henrik@gassmann.onl>
#
# To the extent possible under law, the author(s) have dedicated all
# copyright and related and neighboring rights to this software to the
# public domain worldwide. This software is distributed without any warranty.
#
# You should have received a copy of the CC0 Public Domain Dedication
# along with this software. If not, see
#
#     http://creativecommons.org/publicdomain/zero/1.0/
#
########################################################################
# Tries to find the local libsodium installation.
#
# On Windows the sodium_DIR environment variable is used as a default
# hint which can be overridden by setting the corresponding cmake variable.
#
# Once done the following variables will be defined:
#
#   sodium_FOUND
#   sodium_INCLUDE_DIR
#   sodium_LIBRARY_DEBUG
#   sodium_LIBRARY_RELEASE
#
#
# Furthermore an imported "sodium" target is created.
#

if (CMAKE_C_COMPILER_ID STREQUAL "GNU"
        OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(_GCC_COMPATIBLE 1)
endif()

# static library option
if (NOT DEFINED sodium_USE_STATIC_LIBS)
    option(sodium_USE_STATIC_LIBS "enable to statically link against sodium" OFF)
endif()
if(NOT (sodium_USE_STATIC_LIBS EQUAL sodium_USE_STATIC_LIBS_LAST))
    unset(sodium_LIBRARY CACHE)
    unset(sodium_LIBRARY_DEBUG CACHE)
    unset(sodium_LIBRARY_RELEASE CACHE)
    unset(sodium_DLL_DEBUG CACHE)
    unset(sodium_DLL_RELEASE CACHE)
    set(sodium_USE_STATIC_LIBS_LAST ${sodium_USE_STATIC_LIBS} CACHE INTERNAL "internal change tracking variable")
endif()


########################################################################
# UNIX
if (UNIX)
    # import pkg-config
    find_package(PkgConfig QUIET)
    if (PKG_CONFIG_FOUND)
        pkg_check_modules(sodium_PKG QUIET libsodium)
    endif()

    if(sodium_USE_STATIC_LIBS)
        foreach(_libname ${sodium_PKG_STATIC_LIBRARIES})
            if (NOT _libname MATCHES "^lib.*\\.a$") # ignore strings already ending with .a
                list(INSERT sodium_PKG_STATIC_LIBRARIES 0 "lib${_libname}.a")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES sodium_PKG_STATIC_LIBRARIES)

        # if pkgconfig for libsodium doesn't provide
        # static lib info, then override PKG_STATIC here..
        if (NOT sodium_PKG_STATIC_FOUND)
            set(sodium_PKG_STATIC_LIBRARIES libsodium.a)
        endif()

        set(XPREFIX sodium_PKG_STATIC)
    else()
        if (NOT sodium_PKG_FOUND)
            set(sodium_PKG_LIBRARIES sodium)
        endif()

        set(XPREFIX sodium_PKG)
    endif()

    find_path(sodium_INCLUDE_DIR sodium.h
            HINTS ${${XPREFIX}_INCLUDE_DIRS}
            )
    find_library(sodium_LIBRARY_DEBUG NAMES ${${XPREFIX}_LIBRARIES}
            HINTS ${${XPREFIX}_LIBRARY_DIRS}
            )
    find_library(sodium_LIBRARY_RELEASE NAMES ${${XPREFIX}_LIBRARIES}
            HINTS ${${XPREFIX}_LIBRARY_DIRS}
            )
else()
    message(FATAL_ERROR "this platform is not supported by FindSodium.cmake")
endif()


########################################################################
# common stuff

# extract sodium version
if (sodium_INCLUDE_DIR)
    set(_VERSION_HEADER "${sodium_INCLUDE_DIR}/sodium/version.h")
    if (EXISTS "${_VERSION_HEADER}")
        file(READ "${_VERSION_HEADER}" _VERSION_HEADER_CONTENT)
        string(REGEX REPLACE ".*#[ \t]*define[ \t]*SODIUM_VERSION_STRING[ \t]*\"([^\n]*)\".*" "\\1"
                sodium_VERSION "${_VERSION_HEADER_CONTENT}")
        set(sodium_VERSION "${sodium_VERSION}")
        message(STATUS "Found libsodium version ${sodium_VERSION}")

        # extract the sodium library version
        string(REGEX REPLACE ".*#[ \t]*define[ \t]*SODIUM_LIBRARY_VERSION_MAJOR[ \t]*([^\n]*).*" "\\1"
                sodium_LIBRARY_VERSION_MAJOR "${_VERSION_HEADER_CONTENT}")
        string(REGEX REPLACE ".*#[ \t]*define[ \t]*SODIUM_LIBRARY_VERSION_MINOR[ \t]*([^\n]*).*" "\\1"
                sodium_LIBRARY_VERSION_MINOR "${_VERSION_HEADER_CONTENT}")
        set(sodium_LIBRARY_VERSION "${sodium_LIBRARY_VERSION_MAJOR}.${sodium_LIBRARY_VERSION_MINOR}")
        message(STATUS "libsodium library version found: ${sodium_LIBRARY_VERSION}")
        if (${sodium_LIBRARY_VERSION} VERSION_GREATER "10.0")
            message(FATAL_ERROR "Unsupported libsodium library version ${sodium_LIBRARY_VERSION}!")
        endif()

    endif()
endif()

# communicate results
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
        Sodium # The name must be either uppercase or match the filename case.
        REQUIRED_VARS
        sodium_LIBRARY_RELEASE
        sodium_LIBRARY_DEBUG
        sodium_INCLUDE_DIR
        VERSION_VAR
        sodium_VERSION
)

if(Sodium_FOUND)
    set(sodium_LIBRARIES
            optimized ${sodium_LIBRARY_RELEASE} debug ${sodium_LIBRARY_DEBUG})
endif()

# mark file paths as advanced
mark_as_advanced(sodium_INCLUDE_DIR)
mark_as_advanced(sodium_LIBRARY_DEBUG)
mark_as_advanced(sodium_LIBRARY_RELEASE)

# create imported target
if(sodium_USE_STATIC_LIBS)
    set(_LIB_TYPE STATIC)
else()
    set(_LIB_TYPE SHARED)
endif()
add_library(sodium ${_LIB_TYPE} IMPORTED)

set_target_properties(sodium PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${sodium_INCLUDE_DIR}"
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        )

if (sodium_USE_STATIC_LIBS)
    set_target_properties(sodium PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "SODIUM_STATIC"
            IMPORTED_LOCATION "${sodium_LIBRARY_RELEASE}"
            IMPORTED_LOCATION_DEBUG "${sodium_LIBRARY_DEBUG}"
            )
else()
    if (UNIX)
        set_target_properties(sodium PROPERTIES
                IMPORTED_LOCATION "${sodium_LIBRARY_RELEASE}"
                IMPORTED_LOCATION_DEBUG "${sodium_LIBRARY_DEBUG}"
                )
    endif()
endif()