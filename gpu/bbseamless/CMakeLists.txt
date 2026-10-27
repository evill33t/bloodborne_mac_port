# SPDX-License-Identifier: GPL-2.0-or-later
# Standalone x86-64 client module. No server or graphics backend is built here.
cmake_minimum_required(VERSION 3.24)
project(bbseamless LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    message(FATAL_ERROR "The seamless client runs in the x86-64 game process")
endif()
find_package(fmt REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(Zydis CONFIG QUIET)
if(NOT Zydis_FOUND)
    if(NOT BB_ZYDIS_SOURCE)
        message(FATAL_ERROR "Install Zydis or set BB_ZYDIS_SOURCE to the pinned source checkout")
    endif()
    set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${BB_ZYDIS_SOURCE}" zydis EXCLUDE_FROM_ALL)
    add_library(Zydis::Zydis ALIAS Zydis)
endif()
find_path(XBYAK_INCLUDE_DIR xbyak/xbyak.h REQUIRED)
set(proto_out "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${proto_out}")
if(NOT BB_PROTOC)
    set(BB_PROTOC protobuf::protoc)
endif()
foreach(proto shadnet coop)
    add_custom_command(OUTPUT "${proto_out}/${proto}.pb.cc" "${proto_out}/${proto}.pb.h"
        COMMAND "${BB_PROTOC}" "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/vendor/${proto}"
            "--cpp_out=${proto_out}" "${CMAKE_CURRENT_SOURCE_DIR}/vendor/${proto}/${proto}.proto"
        DEPENDS "vendor/${proto}/${proto}.proto" VERBATIM)
endforeach()
file(GLOB_RECURSE network CONFIGURE_DEPENDS vendor/core/libraries/network/*.cpp
    vendor/core/libraries/np/*.cpp vendor/shadnet/*.cpp)
list(FILTER network EXCLUDE REGEX
    "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")
file(GLOB game CONFIGURE_DEPENDS vendor/core/bloodborne_*.cpp bbcoop/*.cpp)
add_library(bbseamless SHARED ${network} ${game} network_glue.cpp mac_client.cpp mac_epoll.cpp native_ui.cpp
    text/bbport_game_art.cpp text/bbport_save_editor_items.cpp
    vendor/core/guest_hook.cpp vendor/core/file_format/psf.cpp
    vendor/coop/world_log.cpp vendor/coop/coop_messages.cpp vendor/coop/trade.cpp
    vendor/coop/duel.cpp vendor/coop/link_crypto.cpp
    vendor/common/decoder.cpp vendor/common/io_file.cpp vendor/common/string_util.cpp
    vendor/common/thread.cpp vendor/common/error.cpp vendor/common/number_utils.cpp
    include/common/logging/log.cpp include/common/assert.cpp
    "${proto_out}/shadnet.pb.cc" "${proto_out}/coop.pb.cc")
target_include_directories(bbseamless PRIVATE include . vendor "${proto_out}"
    "${XBYAK_INCLUDE_DIR}" ../shadps4 ../third_party/httplib ../third_party/imgui ../third_party/half/include)
target_compile_definitions(bbseamless PRIVATE BOOST_ASIO_STANDALONE NDEBUG
    BLOODBORNE_NATIVE_PORT=1 BLOODBORNE_PARTY_DEATH_ALWAYS=1 BLOODBORNE_PARTY_HUD=1
    CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT)
target_compile_options(bbseamless PRIVATE -fvisibility-inlines-hidden
    -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_libraries(bbseamless PRIVATE fmt::fmt protobuf::libprotobuf
    nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Zydis::Zydis miniz::miniz)
# Content identity also distinguishes local builds made before a commit. No credentials or game files.
file(GLOB_RECURSE identity_inputs CONFIGURE_DEPENDS *.cpp *.c *.h *.hpp *.proto *.exports CMakeLists.txt)
list(SORT identity_inputs)
set(identity_material "")
foreach(input IN LISTS identity_inputs)
    file(SHA256 "${input}" source_hash)
    file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${input}")
    string(APPEND identity_material "${relative}:${source_hash}\n")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${identity_inputs})
string(SHA256 identity_hash "${identity_material}")
string(SUBSTRING "${identity_hash}" 0 12 port_revision)
file(CONFIGURE OUTPUT "${proto_out}/bbcoop_revision.h"
    CONTENT "#pragma once\n#define BB_SCM_REV \"@port_revision@\"\n" @ONLY)
if(APPLE)
    target_link_libraries(bbseamless PRIVATE "-framework Security" "-framework CoreFoundation")
    target_link_options(bbseamless PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/bbseamless.exports")
    set_property(TARGET bbseamless APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bbseamless.exports")
endif()
set_target_properties(bbseamless PROPERTIES BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    INTERPROCEDURAL_OPTIMIZATION OFF)
