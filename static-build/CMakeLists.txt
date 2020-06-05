cmake_minimum_required(VERSION 2.8)

project(tarantool-env NONE)

include(ExternalProject)
set(OPENSSL_VERSION 1.1.1f)
set(ZLIB_VERSION 1.2.11)
set(NCURSES_VERSION 6.2)
set(READLINE_VERSION 8.0)
set(UNWIND_VERSION 1.3-rc1)

find_program(C_COMPILER gcc)
find_program(CXX_COMPILER g++)
set(CMAKE_C_COMPILER ${C_COMPILER})
set(CMAKE_CXX_COMPILER ${CXX_COMPILER})

#
# OpenSSL
#
ExternalProject_Add(openssl
    URL https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz
    CONFIGURE_COMMAND CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        <SOURCE_DIR>/config
        --prefix=<INSTALL_DIR>
        --libdir=lib
        no-shared
    INSTALL_COMMAND ${CMAKE_MAKE_PROGRAM} install_sw
)

#
# ICU
#
ExternalProject_Add(icu
    URL https://github.com/unicode-org/icu/releases/download/release-62-1/icu4c-62_1-src.tgz
    CONFIGURE_COMMAND CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        <SOURCE_DIR>/source/configure
        --with-data-packaging=static
        --prefix=<INSTALL_DIR>
        --disable-shared
        --enable-static
)

#
# ZLIB
#
ExternalProject_Add(zlib
    URL https://zlib.net/zlib-${ZLIB_VERSION}.tar.gz
    CONFIGURE_COMMAND CC=${CMAKE_C_COMPILER}
        <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR>
        --static
    TEST_COMMAND ${CMAKE_MAKE_PROGRAM} check
)

#
# Ncurses
#
ExternalProject_Add(ncurses
    URL https://ftp.gnu.org/gnu/ncurses/ncurses-${NCURSES_VERSION}.tar.gz
    CONFIGURE_COMMAND CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR>
)

#
# ReadLine
#
ExternalProject_Add(readline
    URL https://ftp.gnu.org/gnu/readline/readline-${READLINE_VERSION}.tar.gz
    CONFIGURE_COMMAND CC=${CMAKE_C_COMPILER}
        <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR>
        --disable-shared
    # STEP_TARGETS download
)

#
# ICONV
#
if (NOT APPLE)
    # In linux iconv is embedded into glibc
    # So we find system header and copy it locally
    find_path(ICONV_INCLUDE_DIR iconv.h)
    if(NOT ICONV_INCLUDE_DIR)
        message(FATAL_ERROR "iconv include header not found")
    endif()

    set(ICONV_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/iconv-prefix")

    add_custom_command(
        OUTPUT "${ICONV_INSTALL_PREFIX}/include/iconv.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${ICONV_INSTALL_PREFIX}/include"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${ICONV_INCLUDE_DIR}/iconv.h"
            "${ICONV_INSTALL_PREFIX}/include/iconv.h"
    )
    add_custom_target(iconv
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/iconv-prefix/include/iconv.h"
    )
    set_target_properties(iconv
        PROPERTIES _EP_INSTALL_DIR ${ICONV_INSTALL_PREFIX}
    )
else()
    ExternalProject_Add(iconv
        URL https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.16.tar.gz
        CONFIGURE_COMMAND CC=${CMAKE_C_COMPILER}
            <SOURCE_DIR>/configure
            --prefix=<INSTALL_DIR>
            --disable-shared
            --enable-static
            --with-gnu-ld
        STEP_TARGETS download
    )
endif()

#
# Unwind
#
if (APPLE)
    # On macOS libunwind is a part of MacOSX.sdk
    # So we need to find library and header and
    # copy it locally
    find_path(UNWIND_INCLUDE_DIR libunwind.h)
    find_library(UNWIND_LIBRARY libunwind.dylib
        PATH_SUFFIXES system
    )

    set(UNWIND_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/unwind-prefix")

    set(UNWIND_DEPENDENCIES)

    if (UNWIND_INCLUDE_DIR AND UNWIND_LIBRARY)
        add_custom_command(
            OUTPUT "${UNWIND_INSTALL_PREFIX}/include/unwind.h"
            OUTPUT "${UNWIND_INSTALL_PREFIX}/include/libunwind.h"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${UNWIND_INSTALL_PREFIX}/include"
            COMMAND ${CMAKE_COMMAND} -E copy
                "${UNWIND_INCLUDE_DIR}/libunwind.h"
                "${UNWIND_INCLUDE_DIR}/unwind.h"
                "${UNWIND_INSTALL_PREFIX}/include/"
        )
        add_custom_command(
            OUTPUT "${UNWIND_INSTALL_PREFIX}/lib/libunwind.dylib"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${UNWIND_INSTALL_PREFIX}/lib"
            COMMAND ${CMAKE_COMMAND} -E copy
                "${UNWIND_LIBRARY}"
                "${UNWIND_INSTALL_PREFIX}/lib/"
        )
        set(UNWIND_DEPENDENCIES
            ${UNWIND_DEPENDENCIES}
            "${UNWIND_INSTALL_PREFIX}/lib/libunwind.dylib"
            "${UNWIND_INSTALL_PREFIX}/include/libunwind.h"
        )
    else()
        message(STATUS "Unwind not found")
    endif()

    add_custom_target(unwind DEPENDS ${UNWIND_DEPENDENCIES})
    set_target_properties(unwind
        PROPERTIES _EP_INSTALL_DIR ${UNWIND_INSTALL_PREFIX}
    )
else()
    ExternalProject_Add(unwind
        URL https://download.savannah.nongnu.org/releases/libunwind/libunwind-${UNWIND_VERSION}.tar.gz
        CONFIGURE_COMMAND CC=${CMAKE_C_COMPILER}
            CXX=${CMAKE_CXX_COMPILER}
            <SOURCE_DIR>/configure
            --prefix=<INSTALL_DIR>
            --disable-shared
            --enable-static
        STEP_TARGETS download
    )
endif()

foreach(PROJ openssl icu zlib ncurses readline iconv unwind)
    ExternalProject_Get_Property(${PROJ} install_dir)
    set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH}:${install_dir})
    set(TARANTOOL_DEPENDS ${PROJ} ${TARANTOOL_DEPENDS})
    message(STATUS "Add external project ${PROJ} in ${install_dir}")
endforeach()

ExternalProject_Add(tarantool
    DEPENDS ${TARANTOOL_DEPENDS}
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..
    LIST_SEPARATOR :
    CMAKE_ARGS
        # Override LOCALSTATEDIR to avoid cmake "special" cases:
        # https://cmake.org/cmake/help/v3.4/module/GNUInstallDirs.html#special-cases
        -DCMAKE_INSTALL_LOCALSTATEDIR=<INSTALL_DIR>/var
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
        -DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=FALSE
        -DOPENSSL_USE_STATIC_LIBS=TRUE
        -DCMAKE_BUILD_TYPE=Debug
        -DBUILD_STATIC=TRUE
        -DENABLE_DIST=TRUE
        -DENABLE_BACKTRACE=TRUE
        -DPACKAGE:STRING=${PACKAGE_NAME}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        ${CMAKE_TARANTOOL_ARGS}
    BUILD_COMMAND ${CMAKE_MAKE_PROGRAM} -j
    STEP_TARGETS build
)

enable_testing()
ExternalProject_Get_Property(tarantool binary_dir)
SET(TARANTOOL_BINARY_DIR ${binary_dir})

add_test(
    NAME check-dependencies
    COMMAND ${CMAKE_COMMAND}
        -D FILE=${TARANTOOL_BINARY_DIR}/src/tarantool
        -P CheckDependencies.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test
)

add_test(
    NAME test-run-static
    COMMAND ./test-run.py --builddir ${TARANTOOL_BINARY_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test
)
