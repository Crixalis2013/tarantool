# A macro to build the bundled libcurl
macro(curl_build)
    set(LIBCURL_SOURCE_DIR ${PROJECT_SOURCE_DIR}/third_party/curl)
    set(LIBCURL_BINARY_DIR ${PROJECT_BINARY_DIR}/build/curl/work)
    set(LIBCURL_INSTALL_DIR ${PROJECT_BINARY_DIR}/build/curl/dest)
    set(LIBCURL_CMAKE_FLAGS "")

    if (BUILD_STATIC)
        set(LIBZ_LIB_NAME libz.a)
    else()
        set(LIBZ_LIB_NAME z)
    endif()
    find_library(LIBZ_LIBRARY NAMES ${LIBZ_LIB_NAME})
    if ("${LIBZ_LIBRARY}" STREQUAL "LIBZ_LIBRARY-NOTFOUND")
        message(FATAL_ERROR "Unable to find zlib")
    endif()

    # add librt for clock_gettime function definition
    if(${CMAKE_MAJOR_VERSION} VERSION_LESS "3")
        CHECK_LIBRARY_EXISTS (rt clock_gettime "" HAVE_LIBRT)
        if (HAVE_LIBRT)
            list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_CXX_FLAGS=-lrt")
            list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_C_FLAGS=-lrt")
        endif()
    endif()

    # switch on the static build
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCURL_STATICLIB=ON")

    # switch off the shared build
    list(APPEND LIBCURL_CMAKE_FLAGS "-DBUILD_SHARED_LIBS=OFF")

    # let's disable testing for curl to save build time
    list(APPEND LIBCURL_CMAKE_FLAGS "-DBUILD_TESTING=OFF")

    # Setup use of openssl, use the same OpenSSL library
    # for libcurl as is used for tarantool itself.
    get_filename_component(FOUND_OPENSSL_ROOT_DIR ${OPENSSL_INCLUDE_DIR} DIRECTORY)
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_USE_OPENSSL=ON")
    list(APPEND LIBCURL_CMAKE_FLAGS "-DOPENSSL_ROOT_DIR=${FOUND_OPENSSL_ROOT_DIR}")

    # Setup ARES and its library path, use either c-ares bundled
    # with tarantool or libcurl-default threaded resolver.
    if(BUNDLED_LIBCURL_USE_ARES)
        set(ENABLE_ARES "ON")
        list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_FIND_ROOT_PATH=${ARES_INSTALL_DIR}")
    else()
        set(ENABLE_ARES "OFF")
    endif()
    list(APPEND LIBCURL_CMAKE_FLAGS "-DENABLE_ARES=${ENABLE_ARES}")

    # switch off the group of protocols with special flag HTTP_ONLY:
    #   ftp, file, ldap, ldaps, rtsp, dict, telnet, tftp, pop3, imap, smtp
    list(APPEND LIBCURL_CMAKE_FLAGS "-DHTTP_ONLY=ON")

    # additionaly disable some more protocols
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCURL_DISABLE_SMB=ON")
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCURL_DISABLE_GOPHER=ON")
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCURL_DISABLE_CRYPTO_AUTH=ON")

    # switch on ca-fallback feature
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCURL_CA_FALLBACK=ON")

    # Even though we set the external project's install dir
    # below, we still need to pass the corresponding install
    # prefix via cmake arguments.
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_INSTALL_PREFIX=${LIBCURL_INSTALL_DIR}")

    # The default values for the options below are not always
    # "./lib", "./bin"  and "./include", while curl expects them
    # to be.
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_INSTALL_LIBDIR=lib")
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_INSTALL_INCLUDEDIR=include")
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_INSTALL_BINDIR=bin")

    # Pass the same toolchain as is used to build tarantool itself,
    # because they can be incompatible.
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_LINKER=${CMAKE_LINKER}")
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_AR=${CMAKE_AR}")
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_RANLIB=${CMAKE_RANLIB}")
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_NM=${CMAKE_NM}")
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_STRIP=${CMAKE_STRIP}")

    # found that on FreeBSD 12 this setup is needed for app/socket.test.lua
    list(APPEND LIBCURL_CMAKE_FLAGS "-DLDFLAGS=")

    # In hardened mode, which enables -fPIE by default,
    # the cmake checks don't work without -fPIC.
    list(APPEND LIBCURL_CMAKE_FLAGS "-DCMAKE_REQUIRED_FLAGS=-fPIC")

    include(ExternalProject)
    ExternalProject_Add(
        bundled-libcurl-project
        SOURCE_DIR ${LIBCURL_SOURCE_DIR}
        PREFIX ${LIBCURL_INSTALL_DIR}
        DOWNLOAD_DIR ${LIBCURL_BINARY_DIR}
        TMP_DIR ${LIBCURL_BINARY_DIR}/tmp
        STAMP_DIR ${LIBCURL_BINARY_DIR}/stamp
        BINARY_DIR ${LIBCURL_BINARY_DIR}/curl
        CONFIGURE_COMMAND
            cd <BINARY_DIR> && cmake <SOURCE_DIR>
                ${LIBCURL_CMAKE_FLAGS}
        BUILD_COMMAND cd <BINARY_DIR> && $(MAKE) -j
        INSTALL_COMMAND cd <BINARY_DIR> && $(MAKE) install)

    add_library(bundled-libcurl STATIC IMPORTED GLOBAL)
    set_target_properties(bundled-libcurl PROPERTIES IMPORTED_LOCATION
        ${LIBCURL_INSTALL_DIR}/lib/libcurl.a)
    if (BUNDLED_LIBCURL_USE_ARES)
        # Need to build ares first
        add_dependencies(bundled-libcurl-project bundled-ares)
    endif()
    add_dependencies(bundled-libcurl bundled-libcurl-project)

    set(CURL_INCLUDE_DIRS ${LIBCURL_INSTALL_DIR}/include)
    set(CURL_LIBRARIES bundled-libcurl ${LIBZ_LIBRARY})
    if (BUNDLED_LIBCURL_USE_ARES)
        set(CURL_LIBRARIES ${CURL_LIBRARIES} ${ARES_LIBRARIES})
    endif()
    if (TARGET_OS_LINUX OR TARGET_OS_FREEBSD)
        set(CURL_LIBRARIES ${CURL_LIBRARIES} rt)
    endif()

    unset(FOUND_OPENSSL_ROOT_DIR)
    unset(LIBCURL_INSTALL_DIR)
    unset(LIBCURL_BINARY_DIR)
    unset(LIBCURL_SOURCE_DIR)
endmacro(curl_build)
