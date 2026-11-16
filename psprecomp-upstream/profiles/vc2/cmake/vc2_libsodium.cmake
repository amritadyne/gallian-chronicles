# libsodium for online play (host/online): the official MinGW build of the
# pinned release, fetched once into third_party/ and checked against its
# SHA-256 (dependencies/libsodium.lock), linked statically. Licence: ISC
# (third_party/libsodium-1.0.22-mingw/.../LICENSE, shipped beside the game).
#
# CMake's own download is refused by some Windows certificate setups, so the
# archive is fetched with the system's curl (Windows' own TLS), the same way
# as the FFmpeg archive; the hash decides whether it is used.

set(VC2_SODIUM_VERSION 1.0.22)
set(VC2_SODIUM_ARCHIVE "libsodium-${VC2_SODIUM_VERSION}-mingw.tar.gz")
set(VC2_SODIUM_URL "https://github.com/jedisct1/libsodium/releases/download/${VC2_SODIUM_VERSION}-RELEASE/${VC2_SODIUM_ARCHIVE}")
set(VC2_SODIUM_SHA256 1d99e0afaf27bce664249232e9dc628ae6bb7b49f0ab53ad6db372b028a35d9d)
set(VC2_SODIUM_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/libsodium-${VC2_SODIUM_VERSION}-mingw" CACHE PATH
    "Where the libsodium MinGW build is (fetched when missing)")

function(vc2_find_sodium out_include out_library)
    file(GLOB_RECURSE headers "${VC2_SODIUM_DIR}/*/include/sodium.h")
    file(GLOB_RECURSE libraries "${VC2_SODIUM_DIR}/*/lib/libsodium.a")
    set(${out_include} "" PARENT_SCOPE)
    set(${out_library} "" PARENT_SCOPE)
    foreach(header IN LISTS headers)
        if(header MATCHES "win64|x86_64|x64")
            get_filename_component(dir "${header}" DIRECTORY)
            set(${out_include} "${dir}" PARENT_SCOPE)
        endif()
    endforeach()
    foreach(library IN LISTS libraries)
        if(library MATCHES "win64|x86_64|x64")
            set(${out_library} "${library}" PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

function(vc2_fetch_sodium)
    set(archive "${VC2_SODIUM_DIR}/${VC2_SODIUM_ARCHIVE}")
    file(MAKE_DIRECTORY "${VC2_SODIUM_DIR}")
    if(NOT EXISTS "${archive}")
        find_program(VC2_CURL curl)
        if(VC2_CURL)
            message(STATUS "Fetching ${VC2_SODIUM_URL}")
            execute_process(COMMAND "${VC2_CURL}" -L --fail --silent --show-error -o "${archive}" "${VC2_SODIUM_URL}"
                RESULT_VARIABLE result)
        else()
            file(DOWNLOAD "${VC2_SODIUM_URL}" "${archive}" STATUS status)
        endif()
    endif()
    if(NOT EXISTS "${archive}")
        message(FATAL_ERROR "libsodium could not be downloaded from ${VC2_SODIUM_URL}. "
            "Download it by hand into ${VC2_SODIUM_DIR} and configure again.")
    endif()
    file(SHA256 "${archive}" actual)
    if(NOT actual STREQUAL VC2_SODIUM_SHA256)
        file(REMOVE "${archive}")
        message(FATAL_ERROR "${VC2_SODIUM_ARCHIVE} has the wrong SHA-256 (${actual}); removed. "
            "Expected ${VC2_SODIUM_SHA256} (dependencies/libsodium.lock).")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive}" WORKING_DIRECTORY "${VC2_SODIUM_DIR}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Could not unpack ${archive}")
    endif()
endfunction()

function(vc2_link_sodium target)
    vc2_find_sodium(include library)
    if(NOT include OR NOT library)
        vc2_fetch_sodium()
        vc2_find_sodium(include library)
    endif()
    if(NOT include OR NOT library)
        message(FATAL_ERROR "No 64-bit MinGW libsodium found in ${VC2_SODIUM_DIR}")
    endif()
    message(STATUS "libsodium ${VC2_SODIUM_VERSION}: ${library}")
    target_include_directories(${target} PRIVATE "${include}")
    target_compile_definitions(${target} PRIVATE SODIUM_STATIC=1)
    target_link_libraries(${target} PRIVATE "${library}")
    if(WIN32)
        target_link_libraries(${target} PRIVATE advapi32 bcrypt iphlpapi ws2_32)
    endif()
    # The licence beside the game.
    file(GLOB_RECURSE licences "${VC2_SODIUM_DIR}/*/LICENSE")
    list(LENGTH licences count)
    if(count GREATER 0)
        list(GET licences 0 licence)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${licence}" "$<TARGET_FILE_DIR:${target}>/libsodium-LICENSE.txt"
            VERBATIM)
    endif()
endfunction()
