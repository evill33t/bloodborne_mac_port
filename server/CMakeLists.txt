cmake_minimum_required(VERSION 3.16)
project(shadnet VERSION 1.0.0 LANGUAGES CXX)

include(CTest)

# C++20: the co-op code shared with the game (coop/) needs it.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# === Runtime library: must match Qt6 which always uses DLL runtime ===
if(MSVC)
    cmake_policy(SET CMP0091 NEW)
    # /MD (release) or /MDd (debug) — DLL runtime, same as Qt6.
    # protobuf_MSVC_STATIC_RUNTIME=OFF ensures protobuf follows the same setting.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
    add_compile_options(/EHsc /utf-8)
    # Remove /VERBOSE:LIB from link options (noisy, only needed for debugging)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql Concurrent HttpServer)

add_subdirectory(externals)

set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${PROTO_OUT}")

add_custom_command(
    OUTPUT
        "${PROTO_OUT}/shadnet.pb.cc"
        "${PROTO_OUT}/shadnet.pb.h"
    COMMAND $<TARGET_FILE:protoc>
        "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/src"
        "--cpp_out=${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shadnet.proto"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shadnet.proto"
        protoc
    COMMENT "Generating shadnet protobuf sources"
    VERBATIM
)

# Our co-op messages (coop/coop.proto), shared with the game; with the world log (coop/), which the
# game compiles too.
add_custom_command(
    OUTPUT
        "${PROTO_OUT}/coop.pb.cc"
        "${PROTO_OUT}/coop.pb.h"
    COMMAND $<TARGET_FILE:protoc>
        "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/coop"
        "--cpp_out=${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/coop/coop.proto"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/coop/coop.proto"
        protoc
    COMMENT "Generating the co-op protobuf sources"
    VERBATIM
)
set(COOP_SOURCES
    coop/coop_types.h
    coop/world_log.h
    coop/world_log.cpp
    coop/game_link.h
    coop/packet_routes.h
    coop/coop_stream.h
    coop/coop_messages.h
    coop/coop_messages.cpp
    coop/team_lobby.h
    coop/team_lobby.cpp
    coop/trade.h
    coop/trade.cpp
    coop/link_auth.h
    coop/link_crypto.cpp
    coop/invasions.h
    coop/invasions.cpp
    coop/duel.h
    coop/duel.cpp
    "${PROTO_OUT}/coop.pb.cc"
)

set(SOURCES
    src/main.cpp
    src/watchdog.cpp
    src/bloodborne_reports.cpp
    src/webapi_routes_reports.cpp
    src/config.cpp
    src/config.h
    src/server.cpp
    src/server.h
    src/client_session.cpp
    src/client_session.h
    src/protocol.h
    src/stream_extractor.h
    src/database.cpp
    src/database.h
    src/cmd_account.cpp
    src/cmd_friend.cpp
    src/score_types.h
    src/score_db.h
    src/score_db.cpp
    src/score_cache.cpp
    src/score_cache.h
    src/score_files.h
    src/cmd_score.cpp
    src/proto_utils.h
    "${PROTO_OUT}/shadnet.pb.cc"
    # Matchmaking
    src/matching.h
    src/cmd_matching.cpp
    src/link_server.h
    src/link_server.cpp
    src/admission.h
    src/admission.cpp
    src/admin.h
    src/admin.cpp
    src/http_gate.h
    src/http_gate.cpp
    # The games' one link to the server (co-op phase 2)
    src/server_link.h
    src/server_link.cpp
    # A region of several shards (docs/SCALING.md)
    src/region.h
    src/region.cpp
    src/directory_client.h
    src/directory_client.cpp
    # The parties' worlds (co-op phase 3)
    src/bloodborne_world.h
    src/bloodborne_world.cpp
    src/bloodborne_party_boards.h
    src/bloodborne_party_boards.cpp
    ${COOP_SOURCES}
    # WebAPI HTTP/JSON listener
    src/webapi_server.h
    src/webapi_server.cpp
    src/webapi_auth.h
    src/webapi_auth.cpp
    src/webapi_routes_users.h
    src/webapi_routes_users.cpp
    src/webapi_routes_profile.h
    src/webapi_routes_profile.cpp
    src/webapi_routes_presence.h
    src/webapi_routes_presence.cpp
    src/webapi_routes_session.h
    src/webapi_routes_session.cpp
    src/bloodborne_summon_broker.h
    src/bloodborne_summon_broker.cpp
    src/webapi_routes_bloodborne.h
    src/webapi_routes_bloodborne.cpp
    src/webapi_routes_common.h
    # The game's own online services (backend/bb_dummy_server.py's job)
    src/bloodborne_backend_json.h
    src/bloodborne_backend_json.cpp
    src/bloodborne_backend.h
    src/bloodborne_backend.cpp
    src/bloodborne_backend_server.h
    src/bloodborne_backend_server.cpp
    # Read-only stats HTTP/JSON listener
    src/stats_server.h
    src/stats_server.cpp
)

# The HTTP services (WebAPI, the game's backend, the stats page) need Qt 6.8's QHttpServer; with an
# older Qt (Ubuntu 24.04's 6.4) shadNet builds without them: the link, the worlds and the lobby
# only, enough for a load test (docs/HOSTED_SERVER.md).
set(SHADNET_HTTP_SOURCES
    src/webapi_server.cpp
    src/webapi_routes_bloodborne.cpp
    src/webapi_routes_session.cpp
    src/bloodborne_backend_server.cpp
    src/stats_server.cpp
    src/webapi_server.h
    src/bloodborne_backend_server.h
    src/stats_server.h
)
if(Qt6_VERSION VERSION_LESS 6.8)
    message(WARNING "Qt ${Qt6_VERSION} < 6.8: shadnet builds without its HTTP services")
    list(REMOVE_ITEM SOURCES ${SHADNET_HTTP_SOURCES})
endif()

add_executable(shadnet ${SOURCES})
if(Qt6_VERSION VERSION_LESS 6.8)
    target_compile_definitions(shadnet PRIVATE SHADNET_NO_HTTP)
endif()

# The build's revision (bbcoop_revision.h, BB_SCM_REV), refreshed at every build by the script the
# game's build runs: a game must run this server's build (the link's build check).
set(BB_REVISION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../bbport/gpu/bbcoop/revision.cmake")
if(EXISTS "${BB_REVISION_SCRIPT}")
    add_custom_target(shadnet_revision
        COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/include/bbcoop_revision.h
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR} -P "${BB_REVISION_SCRIPT}"
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/include/bbcoop_revision.h)
    add_dependencies(shadnet shadnet_revision)
else()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/include/bbcoop_revision.h"
         "#pragma once\n#define BB_SCM_REV \"0000000\"\n")
endif()
target_include_directories(shadnet PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/include")

# The ss.info template the game's online check reads; the Python backend reads the same file.
set(BLOODBORNE_SS_INFO_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/../backend/ss_info_template.xml"
    CACHE FILEPATH "backend/ss_info_template.xml, built into shadnet")
if(NOT EXISTS "${BLOODBORNE_SS_INFO_TEMPLATE}")
    message(FATAL_ERROR "No ss.info template at ${BLOODBORNE_SS_INFO_TEMPLATE}")
endif()
get_filename_component(BLOODBORNE_SS_INFO_DIR "${BLOODBORNE_SS_INFO_TEMPLATE}" DIRECTORY)
qt_add_resources(shadnet bloodborne_backend
    PREFIX "/backend"
    BASE "${BLOODBORNE_SS_INFO_DIR}"
    FILES "${BLOODBORNE_SS_INFO_TEMPLATE}"
)

target_include_directories(shadnet PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROTO_OUT}"                                    
    "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
)

target_link_libraries(shadnet PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Concurrent
    Qt6::HttpServer
    libprotobuf
)

if(BUILD_TESTING)
    add_executable(test_bloodborne_summon_broker EXCLUDE_FROM_ALL
        tests/test_bloodborne_summon_broker.cpp
        src/bloodborne_summon_broker.cpp
    )
    target_include_directories(test_bloodborne_summon_broker PRIVATE src)
    target_link_libraries(test_bloodborne_summon_broker PRIVATE Qt6::Core)
    add_test(NAME bloodborne_summon_broker COMMAND test_bloodborne_summon_broker)

    add_executable(test_bloodborne_summon_party EXCLUDE_FROM_ALL
        tests/test_bloodborne_summon_party.cpp
        src/bloodborne_summon_broker.cpp
    )
    target_include_directories(test_bloodborne_summon_party PRIVATE src)
    target_link_libraries(test_bloodborne_summon_party PRIVATE Qt6::Core)
    add_test(NAME bloodborne_summon_party COMMAND test_bloodborne_summon_party)

    add_executable(test_bloodborne_backend_json EXCLUDE_FROM_ALL
        tests/test_bloodborne_backend_json.cpp
        src/bloodborne_backend_json.cpp
    )
    target_include_directories(test_bloodborne_backend_json PRIVATE src)
    target_link_libraries(test_bloodborne_backend_json PRIVATE Qt6::Core)
    add_test(NAME bloodborne_backend_json COMMAND test_bloodborne_backend_json)

    add_executable(test_request_address EXCLUDE_FROM_ALL
        tests/test_request_address.cpp
    )
    target_include_directories(test_request_address PRIVATE src)
    add_test(NAME request_address COMMAND test_request_address)

    add_executable(test_stream_extractor EXCLUDE_FROM_ALL
        tests/test_stream_extractor.cpp
    )
    target_include_directories(test_stream_extractor PRIVATE src)
    target_link_libraries(test_stream_extractor PRIVATE Qt6::Core)
    add_test(NAME stream_extractor COMMAND test_stream_extractor)

    add_executable(test_ss_info_path EXCLUDE_FROM_ALL
        tests/test_ss_info_path.cpp
    )
    target_include_directories(test_ss_info_path PRIVATE src)
    add_test(NAME ss_info_path COMMAND test_ss_info_path)

    add_executable(test_server_link EXCLUDE_FROM_ALL
        tests/test_server_link.cpp
        src/server_link.cpp
        coop/link_crypto.cpp
    )
    target_include_directories(test_server_link PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_server_link PRIVATE Qt6::Core)
    target_compile_definitions(test_server_link PRIVATE
        BLOODBORNE_SERVER_LINK_CONTRACTS="${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/server_link_frames.txt")
    add_test(NAME server_link COMMAND test_server_link)

    add_executable(test_region EXCLUDE_FROM_ALL
        tests/test_region.cpp
        src/region.cpp
        src/server_link.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_region PRIVATE
        src
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_region PRIVATE Qt6::Core Qt6::Network libprotobuf)
    target_compile_definitions(test_region PRIVATE
        BLOODBORNE_SERVER_LINK_CONTRACTS="${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/server_link_frames.txt")
    add_test(NAME region COMMAND test_region)

    add_executable(test_packet_routes EXCLUDE_FROM_ALL tests/test_packet_routes.cpp)
    target_include_directories(test_packet_routes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME packet_routes COMMAND test_packet_routes)

    add_executable(test_build_fingerprint EXCLUDE_FROM_ALL tests/test_build_fingerprint.cpp)
    target_include_directories(test_build_fingerprint PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME build_fingerprint COMMAND test_build_fingerprint)

    add_executable(test_rpc_tunnel EXCLUDE_FROM_ALL tests/test_rpc_tunnel.cpp)
    target_include_directories(test_rpc_tunnel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME rpc_tunnel COMMAND test_rpc_tunnel)

    add_executable(test_watchdog EXCLUDE_FROM_ALL tests/test_watchdog.cpp src/watchdog.cpp)
    target_include_directories(test_watchdog PRIVATE src)
    target_link_libraries(test_watchdog PRIVATE Qt6::Core)
    add_test(NAME watchdog COMMAND test_watchdog)

    add_executable(test_reports EXCLUDE_FROM_ALL tests/test_reports.cpp src/bloodborne_reports.cpp)
    target_include_directories(test_reports PRIVATE src)
    target_link_libraries(test_reports PRIVATE Qt6::Core)
    add_test(NAME reports COMMAND test_reports)

    add_executable(test_bloodborne_world EXCLUDE_FROM_ALL
        tests/test_bloodborne_world.cpp
        src/bloodborne_world.cpp
        src/bloodborne_party_boards.cpp
        src/server_link.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_bloodborne_world PRIVATE
        src
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_bloodborne_world PRIVATE Qt6::Core libprotobuf)
    add_test(NAME bloodborne_world COMMAND test_bloodborne_world)

    add_executable(test_world_writer EXCLUDE_FROM_ALL
        tests/test_world_writer.cpp
        src/bloodborne_world.cpp
        src/bloodborne_party_boards.cpp
        src/server_link.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_world_writer PRIVATE
        src
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_world_writer PRIVATE Qt6::Core libprotobuf)
    add_test(NAME world_writer COMMAND test_world_writer)

    add_executable(test_admission EXCLUDE_FROM_ALL tests/test_admission.cpp src/admission.cpp)
    target_include_directories(test_admission PRIVATE src)
    target_link_libraries(test_admission PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME admission COMMAND test_admission)

    add_executable(test_admin EXCLUDE_FROM_ALL
        tests/test_admin.cpp
        src/admin.cpp
        src/admission.cpp
        src/bloodborne_world.cpp
        src/bloodborne_party_boards.cpp
        src/server_link.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_admin PRIVATE
        src
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_admin PRIVATE Qt6::Core Qt6::Network libprotobuf)
    add_test(NAME admin COMMAND test_admin)

    add_executable(test_http_gate EXCLUDE_FROM_ALL tests/test_http_gate.cpp src/http_gate.cpp)
    target_include_directories(test_http_gate PRIVATE src)
    target_link_libraries(test_http_gate PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME http_gate COMMAND test_http_gate)

    add_executable(test_team_lobby EXCLUDE_FROM_ALL
        tests/test_team_lobby.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_team_lobby PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_team_lobby PRIVATE libprotobuf)
    add_test(NAME team_lobby COMMAND test_team_lobby)

    add_executable(test_invasions EXCLUDE_FROM_ALL tests/test_invasions.cpp coop/invasions.cpp)
    target_include_directories(test_invasions PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME invasions COMMAND test_invasions)

    add_executable(test_room_owner EXCLUDE_FROM_ALL tests/test_room_owner.cpp)
    target_include_directories(test_room_owner PRIVATE src)
    target_link_libraries(test_room_owner PRIVATE Qt6::Core)
    add_test(NAME room_owner COMMAND test_room_owner)

    add_executable(test_bloodborne_trade EXCLUDE_FROM_ALL
        tests/test_bloodborne_trade.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_bloodborne_trade PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_bloodborne_trade PRIVATE libprotobuf)
    add_test(NAME bloodborne_trade COMMAND test_bloodborne_trade)

    add_executable(test_bloodborne_duel EXCLUDE_FROM_ALL
        tests/test_bloodborne_duel.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_bloodborne_duel PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_bloodborne_duel PRIVATE libprotobuf)
    add_test(NAME bloodborne_duel COMMAND test_bloodborne_duel)

    add_executable(test_accounts EXCLUDE_FROM_ALL tests/test_accounts.cpp src/database.cpp)
    target_include_directories(test_accounts PRIVATE src)
    target_link_libraries(test_accounts PRIVATE Qt6::Core Qt6::Sql)
    add_test(NAME accounts COMMAND test_accounts)

    # The fuzz targets (tests/fuzz) on seeded random and mutated inputs: the link's datagrams, game
    # frames, our messages into the worlds, the TCP sessions' packets (all of shadnet but main).
    set(SHADNET_FUZZ_SOURCES ${SOURCES}
        tests/fuzz/fuzz_targets.h
        tests/fuzz/fuzz_link.cpp
        tests/fuzz/fuzz_world.cpp
        tests/fuzz/fuzz_session.cpp
    )
    list(REMOVE_ITEM SHADNET_FUZZ_SOURCES src/main.cpp)
    add_executable(test_fuzz_decoders EXCLUDE_FROM_ALL
        tests/test_fuzz_decoders.cpp
        ${SHADNET_FUZZ_SOURCES}
    )
    target_include_directories(test_fuzz_decoders PRIVATE
        src
        tests
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/include"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_fuzz_decoders PRIVATE
        Qt6::Core Qt6::Network Qt6::Sql Qt6::Concurrent Qt6::HttpServer libprotobuf)
    target_compile_definitions(test_fuzz_decoders PRIVATE
        SHADNET_FUZZ_REGRESSIONS="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/regressions")
    if(Qt6_VERSION VERSION_LESS 6.8)
        target_compile_definitions(test_fuzz_decoders PRIVATE SHADNET_NO_HTTP)
    endif()
    if(TARGET shadnet_revision)
        add_dependencies(test_fuzz_decoders shadnet_revision) # link_server.cpp: bbcoop_revision.h
    endif()
    add_test(NAME fuzz_decoders COMMAND test_fuzz_decoders)
    set_tests_properties(fuzz_decoders PROPERTIES TIMEOUT 600)

    # The backend in shadnet against backend/bb_dummy_server.py: the same requests, the same
    # answers (needs Python 3).
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME bloodborne_backend_parity
            COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_parity
            WORKING_DIRECTORY "${BLOODBORNE_SS_INFO_DIR}/tests")
        set_tests_properties(bloodborne_backend_parity PROPERTIES
            ENVIRONMENT "SHADNET_EXE=$<TARGET_FILE:shadnet>")
        add_test(NAME http_gate_routes
            COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_http_gate_routes
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        set_tests_properties(http_gate_routes PROPERTIES
            ENVIRONMENT "SHADNET_EXE=$<TARGET_FILE:shadnet>")
        # The region's directory (docs/SCALING.md) on fakeredis; skipped without redis-py and
        # fakeredis[lua].
        add_test(NAME region_directory
            COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_directory
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/directory")
    endif()
endif()

# Simulated players for a load test (loadtest/bbload.cpp, docs/HOSTED_SERVER.md).
add_executable(bbload EXCLUDE_FROM_ALL
    loadtest/bbload.cpp
    src/server_link.cpp
    ${COOP_SOURCES}
)
target_include_directories(bbload PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROTO_OUT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
)
target_link_libraries(bbload PRIVATE Qt6::Core Qt6::Network libprotobuf)

# libFuzzer targets (tests/fuzz/libfuzzer_main.cpp), with clang: -DSHADNET_FUZZ=ON
# -DCMAKE_CXX_COMPILER=clang++ in a build folder of their own. Each fuzz_<name> runs one target
# under AddressSanitizer and UndefinedBehaviorSanitizer; `fuzz_<name> -seeds=<folder>` writes its
# seed corpus (docs/HOSTED_SERVER.md).
option(SHADNET_FUZZ "Build the libFuzzer targets (clang)" OFF)
if(SHADNET_FUZZ)
    set(SHADNET_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer -g)
    set(SHADNET_FUZZ_ALL ${SOURCES}
        tests/fuzz/fuzz_link.cpp
        tests/fuzz/fuzz_world.cpp
        tests/fuzz/fuzz_session.cpp
    )
    list(REMOVE_ITEM SHADNET_FUZZ_ALL src/main.cpp)
    # One library of everything, instrumented for coverage; each target its own main.
    add_library(shadnet_fuzz_core STATIC ${SHADNET_FUZZ_ALL})
    target_compile_options(shadnet_fuzz_core PUBLIC
        -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer -g)
    target_link_options(shadnet_fuzz_core PUBLIC -fsanitize=address,undefined)
    target_include_directories(shadnet_fuzz_core PUBLIC
        src
        tests
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(shadnet_fuzz_core PUBLIC
        Qt6::Core Qt6::Network Qt6::Sql Qt6::Concurrent Qt6::HttpServer libprotobuf)
    if(Qt6_VERSION VERSION_LESS 6.8)
        target_compile_definitions(shadnet_fuzz_core PUBLIC SHADNET_NO_HTTP)
    endif()
    foreach(target link game_frame world session)
        if(target STREQUAL "link")
            set(fuzz_function LinkDatagrams)
            set(fuzz_seeds LinkSeeds)
        elseif(target STREQUAL "game_frame")
            set(fuzz_function GameFrames)
            set(fuzz_seeds GameFrameSeeds)
        elseif(target STREQUAL "world")
            set(fuzz_function WorldMessages)
            set(fuzz_seeds WorldSeeds)
        else()
            set(fuzz_function SessionPackets)
            set(fuzz_seeds SessionSeeds)
        endif()
        add_executable(fuzz_${target} tests/fuzz/libfuzzer_main.cpp)
        target_compile_definitions(fuzz_${target} PRIVATE
            SHADNET_FUZZ_TARGET=${fuzz_function} SHADNET_FUZZ_SEEDS=${fuzz_seeds})
        target_compile_options(fuzz_${target} PRIVATE ${SHADNET_FUZZ_FLAGS})
        target_link_options(fuzz_${target} PRIVATE ${SHADNET_FUZZ_FLAGS})
        target_link_libraries(fuzz_${target} PRIVATE shadnet_fuzz_core)
    endforeach()
endif()

add_custom_command(TARGET shadnet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/worlds.cfg"
        "$<TARGET_FILE_DIR:shadnet>/worlds.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scoreboards.cfg"
        "$<TARGET_FILE_DIR:shadnet>/scoreboards.cfg"
)

install(TARGETS shadnet RUNTIME DESTINATION bin)
install(FILES worlds.cfg scoreboards.cfg DESTINATION bin)