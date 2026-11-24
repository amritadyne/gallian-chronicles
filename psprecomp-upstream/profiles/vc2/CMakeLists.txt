cmake_minimum_required(VERSION 3.20)
project(VC2Native VERSION 0.1.0 LANGUAGES CXX)

set(VC2_PORTABLEKIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/PortableKit" CACHE PATH
    "Path to the pinned PortableKit checkout")
if(NOT EXISTS "${VC2_PORTABLEKIT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "PortableKit is required at ${VC2_PORTABLEKIT_DIR}. See dependencies/portablekit.lock.")
endif()

add_subdirectory("${VC2_PORTABLEKIT_DIR}" portablekit)

# The game's step, made fractional at high frame rates, is truncated to a whole
# number in 26 places; those become calls to vc2_step_trunc (host/vc2_tick.cpp).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/vc2_step_sites.cmake")
vc2_patch_step_sites("${CMAKE_CURRENT_SOURCE_DIR}/generated")
vc2_patch_lod_site("${CMAKE_CURRENT_SOURCE_DIR}/generated")
vc2_patch_camera_sites("${CMAKE_CURRENT_SOURCE_DIR}/generated")
# The US release's corpus (generated-variants/us, a library of its own) gets
# the same sites at its own addresses; its calls reach the profile through
# host/vc2_corpus_shim.cpp (built into that library, below).
set(VC2_US_CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/generated-variants/us")
if(EXISTS "${VC2_US_CORPUS}/generated_registry.cpp")
    set(VC2_EDITION us)
    message(STATUS "VC2 sites in the US corpus:")
    vc2_patch_step_sites("${VC2_US_CORPUS}")
    vc2_patch_lod_site("${VC2_US_CORPUS}")
    vc2_patch_camera_sites("${VC2_US_CORPUS}")
    set(VC2_EDITION eu)
endif()
# The Japanese release's corpus (generated-variants/jp, jp.dll): the same sites
# at its own addresses, a table (cmake/vc2_jp_addresses.cmake).
set(VC2_JP_CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/generated-variants/jp")
if(EXISTS "${VC2_JP_CORPUS}/generated_registry.cpp")
    set(VC2_EDITION jp)
    message(STATUS "VC2 sites in the Japanese corpus:")
    vc2_patch_step_sites("${VC2_JP_CORPUS}")
    vc2_patch_lod_site("${VC2_JP_CORPUS}")
    vc2_patch_camera_sites("${VC2_JP_CORPUS}")
    set(VC2_EDITION eu)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vc2_step_sites.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vc2_jp_addresses.cmake")

portablekit_add_game(VC2Native
    PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCES
        host/vc2_profile.cpp host/vc2_native_gu.cpp host/vc2_region.cpp host/vc2_font.cpp
        host/vc2_tick.cpp host/vc2_census.cpp
        host/vc2_lod.cpp host/vc2_settings.cpp host/vc2_settings_menu.cpp
        host/vc2_camera.cpp host/vc2_controls.cpp host/vc2_glyphs.cpp host/vc2_defaults.cpp host/vc2_discord.cpp host/vc2_text.cpp host/vc2_extras.cpp
        host/vc2_audio.cpp host/vc2_voice.cpp
        host/vc2_online.cpp host/online/online_crypto.cpp host/online/online_invite.cpp
        host/online/online_session.cpp host/online/online_nat.cpp
        host/vc2_cheats.cpp
        host/vc2_corpus_bind.cpp)

# The US and Japanese corpus libraries: the hook forwarders their patched
# sites call.
foreach(edition IN ITEMS us jp)
    if(TARGET VC2Native_corpus_${edition})
        target_sources(VC2Native_corpus_${edition} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/host/vc2_corpus_shim.cpp")
        target_include_directories(VC2Native_corpus_${edition} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/host")
    endif()
endforeach()

# 100% native (native rendering plan, phase 7): every GU entry point, draw and
# display list is handled on the host, and PortableKit's GE emulation (command
# decoding, primitives, patches, the list runner) is left out of the build.
# VC2_NATIVE_GU / VC2_NATIVE_DRAW / VC2_NATIVE_LISTS no longer switch anything.
target_compile_definitions(VC2Native PRIVATE PORTABLEKIT_NO_GE_EMULATION=1 VC2_NATIVE_ONLY=1)

# The executable carries the game's name (the CMake target stays VC2Native).
set_target_properties(VC2Native PROPERTIES OUTPUT_NAME "Valkyria Chronicles II")

# The icon is game artwork and not in the public source tree: without
# resources/vc2native.ico the executable simply has no icon of its own.
if(WIN32 AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/vc2native.ico")
    message(STATUS "VC2 icon: resources/vc2native.ico not found; the executable has no icon of its own")
elseif(WIN32)
    # Do not add the .rc file directly to VC2Native: CMake forwards the full
    # C++ include path to MinGW windres, whose preprocessor mishandles paths
    # containing spaces.  This resource has no headers, so build it separately
    # and link its COFF object.
    set(VC2_ICON_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/VC2Native-icon.res")
    add_custom_command(
        OUTPUT "${VC2_ICON_RESOURCE}"
        COMMAND "${CMAKE_RC_COMPILER}" -O coff
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/VC2Native.rc"
            "${VC2_ICON_RESOURCE}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/VC2Native.rc"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/vc2native.ico"
        VERBATIM)
    set_source_files_properties("${VC2_ICON_RESOURCE}" PROPERTIES
        GENERATED TRUE
        EXTERNAL_OBJECT TRUE)
    target_sources(VC2Native PRIVATE "${VC2_ICON_RESOURCE}")
endif()

# Online play's encryption (host/online): libsodium, fetched and checked once.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/vc2_libsodium.cmake")
vc2_link_sodium(VC2Native)

# vc2-online-code (tools/vc2_online_code.cpp): makes a player's friend code for
# the launcher's Network page, from online play's own code, without the game.
add_executable(VC2OnlineCode tools/vc2_online_code.cpp
    host/online/online_crypto.cpp host/online/online_invite.cpp
    host/online/online_session.cpp host/online/online_nat.cpp)
set_target_properties(VC2OnlineCode PROPERTIES OUTPUT_NAME "vc2-online-code"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_features(VC2OnlineCode PRIVATE cxx_std_20)
target_include_directories(VC2OnlineCode PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/host/online" "${VC2_PORTABLEKIT_DIR}/host")
vc2_link_sodium(VC2OnlineCode)
if(MINGW)
    # One file the launcher can run on any PC: its C++ runtime linked in.
    target_link_options(VC2OnlineCode PRIVATE -static)
endif()
add_dependencies(VC2Native VC2OnlineCode)

# vc2_font.cpp draws text glyphs with Windows GDI.
if(WIN32)
    target_link_libraries(VC2Native PRIVATE gdi32)
endif()

# The MinGW runtime (libgcc, libstdc++, winpthread) beside the executable, so
# the game starts on a PC without MinGW installed. Copied rather than linked
# statically: the US/JP corpus DLLs and the executable then share one C++
# runtime. Licence: GCC Runtime Library Exception (THIRD_PARTY_NOTICES).
if(MINGW)
    get_filename_component(VC2_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if(EXISTS "${VC2_MINGW_BIN}/${dll}")
            add_custom_command(TARGET VC2Native POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VC2_MINGW_BIN}/${dll}" "$<TARGET_FILE_DIR:VC2Native>/${dll}"
                VERBATIM)
        else()
            message(WARNING "MinGW runtime ${dll} not found in ${VC2_MINGW_BIN}; the game will need it on PATH")
        endif()
    endforeach()
endif()
